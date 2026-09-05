#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

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
    bool sprinting = false;
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
        info.version = "0.2.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetRuntimeHandles();
        ResetAuthoredDefaults();

        if (!m_runtime.Load(context, "PlatformerKit", {"Startup.sparkscene", "Scenes/Level01.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_groundEntity = scene.Find("Ground");
                                m_cameraEntity = scene.Find("Main Camera");
                                m_playerEntity = scene.Find("Player");
                                m_platformEntities = {scene.Find("Platform A"), scene.Find("Platform B")};
                                m_coinEntities = {scene.Find("Coin 1"), scene.Find("Coin 2"), scene.Find("Coin 3")};
                                m_hazardEntity = scene.Find("Hazard");
                                m_checkpointEntity = scene.Find("Checkpoint");
                                m_finishEntity = scene.Find("Finish");

                                // Required: the ground the level derives its collision from, the
                                // camera, and every entity the level logic drives. The directional
                                // light is decoration nothing else reads, so it stays local and a
                                // missing one warns instead of failing.
                                if (!HasVisual(scene, m_groundEntity) || !scene.Get<Transform>(m_cameraEntity) ||
                                    !scene.Get<Camera>(m_cameraEntity) || !HasVisual(scene, m_playerEntity) ||
                                    !HasVisual(scene, m_platformEntities[0]) ||
                                    !HasVisual(scene, m_platformEntities[1]) || !HasVisual(scene, m_coinEntities[0]) ||
                                    !HasVisual(scene, m_coinEntities[1]) || !HasVisual(scene, m_coinEntities[2]) ||
                                    !HasVisual(scene, m_hazardEntity) || !HasVisual(scene, m_checkpointEntity) ||
                                    !HasVisual(scene, m_finishEntity))
                                    return false;
                                const uint32_t light = scene.Find("Directional Light");
                                if (!scene.Get<Transform>(light) || !scene.Get<LightComponent>(light))
                                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                                   "PlatformerKit scene has no usable 'Directional Light'");
                                return true;
                            }))
            return false;

        if (m_runtime.IsActive())
            CaptureAuthoredScene();
        RestartLevel();
        if (m_runtime.IsActive() && m_runtime.GetGraphics())
        {
            m_hudEntity = m_runtime.CreateSprite("Platformer Status HUD", "Assets/platformer_runtime_sheet.png",
                                                 Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2));
        }
        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetRuntimeHandles();
        m_sprintInput = false;
        m_context = nullptr;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        UpdateRuntimeInput();
        if (m_state.finished || m_state.lives == 0)
            m_state.sprinting = false;
        if (!m_state.finished && m_state.lives > 0)
        {
            m_fixedAccumulator += std::min(deltaTime, kMaxFrameTime);
            while (m_fixedAccumulator >= kFixedStep && !m_state.finished && m_state.lives > 0)
            {
                m_fixedAccumulator -= kFixedStep;
                SimulateStep(kFixedStep);
            }
        }
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    void SetMoveInput(float input) { m_moveInput = std::isfinite(input) ? std::clamp(input, -1.0f, 1.0f) : 0.0f; }
    void SetSprintInput(bool sprinting) { m_sprintInput = sprinting; }

    bool Jump()
    {
        if (m_state.finished || m_state.lives == 0 || m_state.jumpsUsed >= 2)
            return false;
        ++m_state.jumpsUsed;
        m_state.grounded = false;
        m_state.velocityY = kJumpVelocity;
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
        if (!std::isfinite(x) || !std::isfinite(y))
            return;
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
        m_state.velocityX = 0.0f;
        m_state.velocityY = 0.0f;
        m_state.sprinting = false;
        m_sprintInput = false;
        m_moveInput = 0.0f;
        return true;
    }

    void RestartLevel()
    {
        m_state = {};
        m_state.x = m_playerSpawn.x;
        m_state.y = m_playerSpawn.y;
        m_collected.fill(false);
        m_moveInput = 0.0f;
        m_sprintInput = false;
        m_fixedAccumulator = 0.0f;
    }

    [[nodiscard]] const PlatformerKitState& GetState() const { return m_state; }
    [[nodiscard]] bool IsCoinCollected(std::size_t index) const
    {
        return index < m_collected.size() && m_collected[index];
    }

  private:
    struct Surface
    {
        float centerX = 0.0f;
        float halfWidth = 0.0f;
        float standY = 0.0f;
    };

    struct Trigger
    {
        DirectX::XMFLOAT3 center{};
        DirectX::XMFLOAT3 halfExtents{};
    };

    static constexpr float kFixedStep = 1.0f / 120.0f;
    static constexpr float kMaxFrameTime = 0.25f;
    static constexpr float kMoveSpeed = 8.0f;
    static constexpr float kSprintSpeed = 11.5f;
    static constexpr float kGravity = 24.0f;
    static constexpr float kJumpVelocity = 10.0f;

    static bool HasVisual(const Spark::Templates::TemplateRuntimeScene& scene, uint32_t entity)
    {
        return scene.Get<Transform>(entity) && scene.Get<MeshRenderer>(entity);
    }

    void ResetRuntimeHandles()
    {
        constexpr uint32_t invalid = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_groundEntity = invalid;
        m_cameraEntity = invalid;
        m_playerEntity = invalid;
        m_platformEntities.fill(invalid);
        m_coinEntities.fill(invalid);
        m_hazardEntity = invalid;
        m_checkpointEntity = invalid;
        m_finishEntity = invalid;
        m_hudEntity = invalid;
    }

    void ResetAuthoredDefaults()
    {
        m_playerSpawn = {};
        m_playerDepth = 0.0f;
        m_playerHalfWidth = 0.4f;
        m_playerHalfHeight = 0.8f;
        m_cameraOffset = {0.0f, 5.0f, -12.0f};
        m_surfaces = {Surface{0.0f, 1000.0f, 0.0f}, Surface{}, Surface{}};
        m_coinTriggers = {};
        m_hazardTrigger = {};
        m_checkpointTrigger = {};
        m_finishTrigger = {};
        m_checkpointRespawnY = 0.0f;
        m_killPlaneY = -12.0f;
    }

    static Trigger MakeTrigger(const Transform& transform)
    {
        return {transform.position,
                {std::abs(transform.scale.x) * 0.5f, std::abs(transform.scale.y) * 0.5f,
                 std::abs(transform.scale.z) * 0.5f}};
    }

    void CaptureAuthoredScene()
    {
        const Transform& player = *m_runtime.Get<Transform>(m_playerEntity);
        const Transform& camera = *m_runtime.Get<Transform>(m_cameraEntity);
        const Transform& ground = *m_runtime.Get<Transform>(m_groundEntity);
        m_playerSpawn = player.position;
        m_playerDepth = player.position.z;
        m_playerHalfWidth = std::abs(player.scale.x) * 0.5f;
        m_playerHalfHeight = std::abs(player.scale.y) * 0.5f;
        m_cameraOffset = {camera.position.x - player.position.x, camera.position.y - player.position.y,
                          camera.position.z - player.position.z};
        m_surfaces[0] = {ground.position.x, std::abs(ground.scale.x) * 0.5f, player.position.y};
        m_killPlaneY = ground.position.y - 12.0f;

        for (std::size_t index = 0; index < m_platformEntities.size(); ++index)
        {
            const Transform& platform = *m_runtime.Get<Transform>(m_platformEntities[index]);
            m_surfaces[index + 1] = {platform.position.x, std::abs(platform.scale.x) * 0.5f,
                                     platform.position.y + std::abs(platform.scale.y) * 0.5f + m_playerHalfHeight};
        }
        for (std::size_t index = 0; index < m_coinEntities.size(); ++index)
            m_coinTriggers[index] = MakeTrigger(*m_runtime.Get<Transform>(m_coinEntities[index]));
        m_hazardTrigger = MakeTrigger(*m_runtime.Get<Transform>(m_hazardEntity));
        m_checkpointTrigger = MakeTrigger(*m_runtime.Get<Transform>(m_checkpointEntity));
        m_finishTrigger = MakeTrigger(*m_runtime.Get<Transform>(m_finishEntity));
        m_checkpointRespawnY = HighestSupportAt(m_checkpointTrigger.center.x, m_checkpointTrigger.center.y);
    }

    [[nodiscard]] float HighestSupportAt(float x, float ceilingY) const
    {
        float highest = m_surfaces[0].standY;
        for (const Surface& surface : m_surfaces)
        {
            if (surface.halfWidth > 0.0f && std::abs(x - surface.centerX) <= surface.halfWidth + m_playerHalfWidth &&
                surface.standY <= ceilingY + m_playerHalfHeight && surface.standY > highest)
                highest = surface.standY;
        }
        return highest;
    }

    [[nodiscard]] bool IsStandingOnSurface() const
    {
        for (const Surface& surface : m_surfaces)
        {
            if (surface.halfWidth > 0.0f &&
                std::abs(m_state.x - surface.centerX) <= surface.halfWidth + m_playerHalfWidth &&
                std::abs(m_state.y - surface.standY) <= 0.02f)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool Overlaps(const Trigger& trigger) const
    {
        return std::abs(m_state.x - trigger.center.x) <= trigger.halfExtents.x + m_playerHalfWidth &&
               std::abs(m_state.y - trigger.center.y) <= trigger.halfExtents.y + m_playerHalfHeight;
    }

    void SimulateStep(float deltaTime)
    {
        m_state.elapsedSeconds += deltaTime;
        m_state.sprinting = m_sprintInput && std::abs(m_moveInput) > 0.0f;
        m_state.velocityX = m_moveInput * (m_state.sprinting ? kSprintSpeed : kMoveSpeed);
        if (m_state.grounded && !IsStandingOnSurface())
            m_state.grounded = false;

        const float previousY = m_state.y;
        if (!m_state.grounded)
            m_state.velocityY -= kGravity * deltaTime;
        m_state.x += m_state.velocityX * deltaTime;
        m_state.y += m_state.velocityY * deltaTime;

        if (m_state.velocityY <= 0.0f)
        {
            float landingY = -std::numeric_limits<float>::infinity();
            for (const Surface& surface : m_surfaces)
            {
                const bool horizontal = surface.halfWidth > 0.0f &&
                                        std::abs(m_state.x - surface.centerX) <= surface.halfWidth + m_playerHalfWidth;
                if (horizontal && previousY >= surface.standY - 0.001f && m_state.y <= surface.standY &&
                    surface.standY > landingY)
                    landingY = surface.standY;
            }
            if (std::isfinite(landingY))
            {
                m_state.y = landingY;
                m_state.velocityY = 0.0f;
                m_state.grounded = true;
                m_state.jumpsUsed = 0;
            }
        }

        ProcessTriggers();
        if (m_state.y < m_killPlaneY)
            HitHazard();
    }

    void ProcessTriggers()
    {
        for (std::size_t index = 0; index < m_coinTriggers.size(); ++index)
        {
            if (!m_collected[index] && Overlaps(m_coinTriggers[index]))
                CollectCoin(index);
        }
        if (!m_state.checkpointActive && Overlaps(m_checkpointTrigger))
            ActivateCheckpoint(m_checkpointTrigger.center.x, m_checkpointRespawnY);
        if (Overlaps(m_hazardTrigger))
        {
            HitHazard();
            return;
        }
        if (Overlaps(m_finishTrigger))
            ReachFinish();
    }

    void UpdateRuntimeInput()
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        float horizontal = 0.0f;
        if (input->IsKeyDown('D'))
            horizontal += 1.0f;
        if (input->IsKeyDown('A'))
            horizontal -= 1.0f;
        SetMoveInput(horizontal);
        SetSprintInput(input->IsKeyDown(VK_SHIFT));

        if (input->WasKeyPressed(VK_SPACE))
            Jump();
        if (input->WasKeyPressed('R'))
            RestartLevel();
    }

    void SyncRuntimeState()
    {
        if (Transform* player = m_runtime.Get<Transform>(m_playerEntity))
            player->position = {m_state.x, m_state.y, m_playerDepth};
        if (MeshRenderer* player = m_runtime.Get<MeshRenderer>(m_playerEntity))
            player->visible = m_state.lives > 0;

        for (std::size_t index = 0; index < m_coinEntities.size(); ++index)
        {
            if (Transform* coinTransform = m_runtime.Get<Transform>(m_coinEntities[index]))
            {
                coinTransform->rotation.y =
                    std::fmod(m_state.elapsedSeconds * 120.0f + static_cast<float>(index) * 70.0f, 360.0f);
            }
            if (MeshRenderer* coin = m_runtime.Get<MeshRenderer>(m_coinEntities[index]))
                coin->visible = !m_collected[index];
        }

        if (Transform* checkpoint = m_runtime.Get<Transform>(m_checkpointEntity))
            checkpoint->rotation.y = std::fmod(m_state.elapsedSeconds * 45.0f, 360.0f);
        if (MeshRenderer* checkpoint = m_runtime.Get<MeshRenderer>(m_checkpointEntity))
            checkpoint->emissive = m_state.checkpointActive ? 0.35f : 0.08f;
        if (MeshRenderer* finish = m_runtime.Get<MeshRenderer>(m_finishEntity))
            finish->emissive = m_state.coins == m_collected.size() ? 0.4f : 0.04f;

        if (Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            camera->position = {m_state.x + m_cameraOffset.x, m_state.y + m_cameraOffset.y,
                                m_playerDepth + m_cameraOffset.z};
        }

        if (SpriteRenderer* hud = m_runtime.Get<SpriteRenderer>(m_hudEntity))
        {
            if (m_state.finished)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2);
                hud->color = {0.35f, 1.0f, 0.55f, 0.95f};
            }
            else if (m_state.lives == 0)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 2);
                hud->color = {1.0f, 0.35f, 0.3f, 0.95f};
            }
            else if (m_state.coins == m_collected.size())
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2);
                hud->color = {0.35f, 0.9f, 1.0f, 0.95f};
            }
            else if (m_state.checkpointActive)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 1);
                hud->color = {0.4f, 0.9f, 1.0f, 0.95f};
            }
            else if (m_state.lives < 3)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2);
                hud->color = {1.0f, 0.65f, 0.25f, 0.95f};
            }
            else
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 1);
                hud->color = {1.0f, 0.85f, 0.25f, 0.95f};
            }
        }
        m_runtime.PlaceHud(m_cameraEntity, m_hudEntity, -0.12f, 0.08f, 0.32f, 0.055f, 0.055f);
    }

    void Respawn()
    {
        m_state.x = m_state.checkpointActive ? m_state.checkpointX : m_playerSpawn.x;
        m_state.y = m_state.checkpointActive ? m_state.checkpointY : m_playerSpawn.y;
        m_state.velocityX = 0.0f;
        m_state.velocityY = 0.0f;
        m_state.grounded = std::abs(m_state.y - HighestSupportAt(m_state.x, m_state.y)) <= 0.02f;
        m_state.jumpsUsed = 0;
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    PlatformerKitState m_state;
    std::array<bool, 3> m_collected{};
    std::array<Surface, 3> m_surfaces{};
    std::array<Trigger, 3> m_coinTriggers{};
    Trigger m_hazardTrigger{};
    Trigger m_checkpointTrigger{};
    Trigger m_finishTrigger{};
    DirectX::XMFLOAT3 m_playerSpawn{};
    DirectX::XMFLOAT3 m_cameraOffset{};
    float m_playerDepth = 0.0f;
    float m_playerHalfWidth = 0.4f;
    float m_playerHalfHeight = 0.8f;
    float m_checkpointRespawnY = 0.0f;
    float m_killPlaneY = -12.0f;
    float m_moveInput = 0.0f;
    float m_fixedAccumulator = 0.0f;
    bool m_sprintInput = false;
    uint32_t m_groundEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    std::array<uint32_t, 2> m_platformEntities{};
    std::array<uint32_t, 3> m_coinEntities{};
    uint32_t m_hazardEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_checkpointEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_finishEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
};
