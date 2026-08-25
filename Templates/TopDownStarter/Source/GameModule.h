#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

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
    bool playerDefeated = false;
    bool won = false;
};

class TopDownStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "TopDownStarter";
        info.version = "0.3.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetSceneDefaults();
        ResetEntityHandles();
        m_attackHeld = false;
        m_collectHeld = false;
        m_restartHeld = false;

        if (!m_runtime.Load(context, "TopDownStarter", {"Startup.sparkscene", "Scenes/Skirmish.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_cameraEntity = scene.Find("Main Camera");
                                m_playerEntity = scene.Find("Player");
                                m_enemyEntity = scene.Find("Enemy");
                                m_pickupEntity = scene.Find("Energy Pickup");
                                return scene.Get<Transform>(m_cameraEntity) && scene.Get<Camera>(m_cameraEntity) &&
                                       scene.Get<Transform>(m_playerEntity) &&
                                       scene.Get<MeshRenderer>(m_playerEntity) && scene.Get<Transform>(m_enemyEntity) &&
                                       scene.Get<MeshRenderer>(m_enemyEntity) && scene.Get<Transform>(m_pickupEntity) &&
                                       scene.Get<MeshRenderer>(m_pickupEntity);
                            }))
        {
            ResetEntityHandles();
            m_context = nullptr;
            return false;
        }

        if (m_runtime.IsActive())
            CaptureSceneState();
        Restart();
        if (m_runtime.IsActive() && m_runtime.GetGraphics())
        {
            m_hudEntity = m_runtime.CreateSprite("TopDown Status HUD", "Assets/top_down_runtime_sheet.png",
                                                 Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2));
            m_enemyHudEntity = m_runtime.CreateSprite("TopDown Enemy HUD", "Assets/top_down_runtime_sheet.png",
                                                      Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0));
            m_pickupHudEntity = m_runtime.CreateSprite("TopDown Pickup HUD", "Assets/top_down_runtime_sheet.png",
                                                       Spark::Templates::TemplateRuntimeScene::SheetCell(2, 1));
        }
        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetEntityHandles();
        m_context = nullptr;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        m_enemyAttackCooldown = std::max(0.0f, m_enemyAttackCooldown - deltaTime);
        m_playerHitFlashRemaining = std::max(0.0f, m_playerHitFlashRemaining - deltaTime);
        m_enemyHitFlashRemaining = std::max(0.0f, m_enemyHitFlashRemaining - deltaTime);
        UpdateRuntimeInput(deltaTime);
        if (!m_state.pickupCollected)
            m_pickupTime += deltaTime;
        if (!m_state.enemyDefeated && !m_state.playerDefeated && !m_state.won)
        {
            const float dx = m_state.playerX - m_state.enemyX;
            const float dz = m_state.playerZ - m_state.enemyZ;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > 0.01f)
            {
                const float step = std::min(distance, kEnemyMoveSpeed * deltaTime);
                m_state.enemyX += dx / distance * step;
                m_state.enemyZ += dz / distance * step;
                m_enemyYawDegrees = std::atan2(dx, dz) * kRadiansToDegrees;
            }

            const float remainingDx = m_state.playerX - m_state.enemyX;
            const float remainingDz = m_state.playerZ - m_state.enemyZ;
            if (remainingDx * remainingDx + remainingDz * remainingDz < kEnemyAttackRangeSquared &&
                m_enemyAttackCooldown == 0.0f)
            {
                m_state.playerHealth = std::max(0.0f, m_state.playerHealth - kEnemyDamage);
                m_enemyAttackCooldown = kEnemyAttackInterval;
                m_playerHitFlashRemaining = kHitFlashSeconds;
                if (m_state.playerHealth == 0.0f)
                    m_state.playerDefeated = true;
            }
        }
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    void Move(float xAxis, float zAxis, float deltaTime)
    {
        if (!std::isfinite(xAxis) || !std::isfinite(zAxis) || !std::isfinite(deltaTime) || deltaTime <= 0.0f ||
            m_state.playerHealth <= 0.0f || m_state.playerDefeated || m_state.won)
            return;

        xAxis = std::clamp(xAxis, -1.0f, 1.0f);
        zAxis = std::clamp(zAxis, -1.0f, 1.0f);
        NormalizeAxes(xAxis, zAxis);
        const float previousX = m_state.playerX;
        const float previousZ = m_state.playerZ;
        m_state.playerX = std::clamp(m_state.playerX + xAxis * 6.0f * deltaTime, -9.0f, 9.0f);
        m_state.playerZ = std::clamp(m_state.playerZ + zAxis * 6.0f * deltaTime, -9.0f, 9.0f);
        const float movedX = m_state.playerX - previousX;
        const float movedZ = m_state.playerZ - previousZ;
        if (std::abs(movedX) > 0.0001f || std::abs(movedZ) > 0.0001f)
            m_playerYawDegrees = std::atan2(movedX, movedZ) * kRadiansToDegrees;
        UpdateCameraTracking();
    }

    void PanCamera(float xDelta, float zDelta)
    {
        if (!std::isfinite(xDelta) || !std::isfinite(zDelta))
            return;
        m_cameraPanX = std::clamp(m_cameraPanX + xDelta, -10.0f, 10.0f);
        m_cameraPanZ = std::clamp(m_cameraPanZ + zDelta, -10.0f, 10.0f);
        UpdateCameraTracking();
    }

    void ZoomCamera(float delta)
    {
        if (std::isfinite(delta))
            m_state.cameraHeight = std::clamp(m_state.cameraHeight + delta, 8.0f, 30.0f);
    }

    bool TryCollectPickup()
    {
        if (m_state.pickupCollected || m_state.playerDefeated ||
            PlayerDistanceSquared(m_pickupPosition.x, m_pickupPosition.z) > 2.25f)
        {
            return false;
        }
        m_state.pickupCollected = true;
        m_state.playerHealth = std::min(100.0f, m_state.playerHealth + 25.0f);
        m_playerHitFlashRemaining = 0.0f;
        return true;
    }

    bool AttackEnemy()
    {
        if (m_state.enemyDefeated || m_state.playerDefeated || m_state.playerHealth <= 0.0f ||
            PlayerDistanceSquared(m_state.enemyX, m_state.enemyZ) > 6.25f)
        {
            return false;
        }
        m_state.enemyHealth = std::max(0.0f, m_state.enemyHealth - (m_state.pickupCollected ? 30.0f : 20.0f));
        m_enemyHitFlashRemaining = kHitFlashSeconds;
        if (m_state.enemyHealth == 0.0f)
        {
            m_state.enemyDefeated = true;
            m_state.won = true;
        }
        return true;
    }

    void Restart()
    {
        m_state = {};
        m_state.playerX = m_playerSpawnPosition.x;
        m_state.playerZ = m_playerSpawnPosition.z;
        m_state.enemyX = m_enemySpawnPosition.x;
        m_state.enemyZ = m_enemySpawnPosition.z;
        m_state.cameraHeight = m_cameraSpawnPosition.y;
        m_cameraPanX = m_cameraSpawnPosition.x - m_playerSpawnPosition.x;
        m_cameraPanZ = m_cameraSpawnPosition.z - m_playerSpawnPosition.z;
        m_playerYawDegrees = m_playerSpawnRotation.y;
        m_enemyYawDegrees = m_enemySpawnRotation.y;
        m_enemyAttackCooldown = 0.0f;
        m_playerHitFlashRemaining = 0.0f;
        m_enemyHitFlashRemaining = 0.0f;
        m_pickupTime = 0.0f;
        UpdateCameraTracking();
        SyncRuntimeState();
    }

    [[nodiscard]] const TopDownStarterState& GetState() const { return m_state; }
    [[nodiscard]] float GetEnemyHitFlashRemaining() const { return m_enemyHitFlashRemaining; }

  private:
    static constexpr float kRadiansToDegrees = 57.29577951308232f;
    static constexpr float kEnemyMoveSpeed = 1.5f;
    static constexpr float kEnemyAttackRangeSquared = 1.44f;
    static constexpr float kEnemyDamage = 12.0f;
    static constexpr float kEnemyAttackInterval = 0.8f;
    static constexpr float kHitFlashSeconds = 0.18f;

    static void NormalizeAxes(float& xAxis, float& zAxis)
    {
        const float magnitude = std::sqrt(xAxis * xAxis + zAxis * zAxis);
        if (magnitude > 1.0f)
        {
            xAxis /= magnitude;
            zAxis /= magnitude;
        }
    }

    void UpdateRuntimeInput(float deltaTime)
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        float xAxis = 0.0f;
        float zAxis = 0.0f;
        if (input->IsKeyDown('D'))
            xAxis += 1.0f;
        if (input->IsKeyDown('A'))
            xAxis -= 1.0f;
        if (input->IsKeyDown('W'))
            zAxis += 1.0f;
        if (input->IsKeyDown('S'))
            zAxis -= 1.0f;
        Move(xAxis, zAxis, deltaTime);

        float panX = 0.0f;
        float panZ = 0.0f;
        if (input->IsKeyDown(VK_RIGHT))
            panX += 1.0f;
        if (input->IsKeyDown(VK_LEFT))
            panX -= 1.0f;
        if (input->IsKeyDown(VK_UP))
            panZ += 1.0f;
        if (input->IsKeyDown(VK_DOWN))
            panZ -= 1.0f;
        PanCamera(panX * 6.0f * deltaTime, panZ * 6.0f * deltaTime);
        if (input->IsKeyDown('Q'))
            ZoomCamera(-10.0f * deltaTime);
        if (input->IsKeyDown('E'))
            ZoomCamera(10.0f * deltaTime);

        const bool attackDown = input->IsKeyDown(VK_SPACE);
        if (attackDown && !m_attackHeld)
            AttackEnemy();
        m_attackHeld = attackDown;

        const bool collectDown = input->IsKeyDown('F');
        if (collectDown && !m_collectHeld)
            TryCollectPickup();
        m_collectHeld = collectDown;

        const bool restartDown = input->IsKeyDown('R');
        if (restartDown && !m_restartHeld)
            Restart();
        m_restartHeld = restartDown;
    }

    void CaptureSceneState()
    {
        if (const Transform* player = m_runtime.Get<Transform>(m_playerEntity))
        {
            m_playerSpawnPosition = player->position;
            m_playerSpawnRotation = player->rotation;
        }
        if (const Transform* enemy = m_runtime.Get<Transform>(m_enemyEntity))
        {
            m_enemySpawnPosition = enemy->position;
            m_enemySpawnRotation = enemy->rotation;
        }
        if (const Transform* pickup = m_runtime.Get<Transform>(m_pickupEntity))
            m_pickupPosition = pickup->position;
        if (const Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            m_cameraSpawnPosition = camera->position;
            m_cameraPitchDegrees = std::clamp(camera->rotation.x, -89.0f, 89.0f);
            m_cameraYawDegrees = camera->rotation.y;
        }
    }

    void SyncRuntimeState()
    {
        if (Transform* player = m_runtime.Get<Transform>(m_playerEntity))
        {
            player->position = {m_state.playerX, m_playerSpawnPosition.y, m_state.playerZ};
            player->rotation.y = m_playerYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_playerEntity))
                mesh->worldMatrixDirty = true;
        }
        if (Transform* enemy = m_runtime.Get<Transform>(m_enemyEntity))
        {
            enemy->position = {m_state.enemyX, m_enemySpawnPosition.y, m_state.enemyZ};
            enemy->rotation.y = m_enemyYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_enemyEntity))
                mesh->worldMatrixDirty = true;
        }
        if (Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            camera->position = {m_state.cameraX, m_state.cameraHeight, m_state.cameraZ};
            camera->rotation = {m_cameraPitchDegrees, m_cameraYawDegrees, 0.0f};
        }
        if (MeshRenderer* player = m_runtime.Get<MeshRenderer>(m_playerEntity))
        {
            player->visible = !m_state.playerDefeated;
            player->emissive = m_playerHitFlashRemaining > 0.0f ? 0.85f : 0.05f;
        }
        if (MeshRenderer* enemy = m_runtime.Get<MeshRenderer>(m_enemyEntity))
        {
            enemy->visible = !m_state.enemyDefeated;
            enemy->emissive = m_enemyHitFlashRemaining > 0.0f ? 0.95f : 0.08f;
        }
        if (MeshRenderer* pickup = m_runtime.Get<MeshRenderer>(m_pickupEntity))
            pickup->visible = !m_state.pickupCollected;
        if (Transform* pickup = m_runtime.Get<Transform>(m_pickupEntity))
        {
            pickup->position.y = m_pickupPosition.y + 0.15f * std::sin(m_pickupTime * 2.5f);
            pickup->rotation.y = std::fmod(m_pickupTime * 70.0f, 360.0f);
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_pickupEntity))
                mesh->worldMatrixDirty = true;
        }

        if (SpriteRenderer* hud = m_runtime.Get<SpriteRenderer>(m_hudEntity))
        {
            if (m_state.won)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2);
                hud->color = {0.3f, 1.0f, 0.55f, 0.95f};
            }
            else if (m_state.playerDefeated)
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 2);
                hud->color = {1.0f, 0.25f, 0.2f, 0.95f};
            }
            else
            {
                hud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2);
                hud->color = {1.0f, 1.0f, 1.0f, 0.95f};
            }
        }
        if (SpriteRenderer* enemyHud = m_runtime.Get<SpriteRenderer>(m_enemyHudEntity))
        {
            const float healthRatio = std::clamp(m_state.enemyHealth / 75.0f, 0.0f, 1.0f);
            enemyHud->color = {1.0f, 0.25f + 0.75f * healthRatio, 0.25f + 0.75f * healthRatio, 0.95f};
            enemyHud->visible = !m_state.enemyDefeated && !m_state.playerDefeated;
        }
        if (SpriteRenderer* pickupHud = m_runtime.Get<SpriteRenderer>(m_pickupHudEntity))
        {
            pickupHud->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(
                m_state.pickupCollected ? 1u : 2u, m_state.pickupCollected ? 2u : 1u);
            pickupHud->color = m_state.pickupCollected ? DirectX::XMFLOAT4{0.35f, 1.0f, 0.55f, 0.95f}
                                                       : DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 0.95f};
        }
        m_runtime.PlaceHud(m_cameraEntity, m_hudEntity, -0.13f, 0.09f, 0.32f, 0.085f, 0.055f);
        m_runtime.PlaceHud(m_cameraEntity, m_enemyHudEntity, 0.0f, 0.09f, 0.32f, 0.055f, 0.055f);
        m_runtime.PlaceHud(m_cameraEntity, m_pickupHudEntity, 0.13f, 0.09f, 0.32f, 0.055f, 0.055f);
    }

    void UpdateCameraTracking()
    {
        m_state.cameraX = m_state.playerX + m_cameraPanX;
        m_state.cameraZ = m_state.playerZ + m_cameraPanZ;
    }

    [[nodiscard]] float PlayerDistanceSquared(float x, float z) const
    {
        const float dx = m_state.playerX - x;
        const float dz = m_state.playerZ - z;
        return dx * dx + dz * dz;
    }

    void ResetSceneDefaults()
    {
        m_playerSpawnPosition = {0.0f, 0.5f, 0.0f};
        m_playerSpawnRotation = {};
        m_enemySpawnPosition = {6.0f, 0.5f, 4.0f};
        m_enemySpawnRotation = {};
        m_pickupPosition = {-4.0f, 0.35f, 3.0f};
        m_cameraSpawnPosition = {0.0f, 20.0f, 0.0f};
        m_cameraPitchDegrees = 89.0f;
        m_cameraYawDegrees = 0.0f;
    }

    void ResetEntityHandles()
    {
        m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_enemyEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_pickupEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_enemyHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_pickupHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    TopDownStarterState m_state;
    DirectX::XMFLOAT3 m_playerSpawnPosition{0.0f, 0.5f, 0.0f};
    DirectX::XMFLOAT3 m_playerSpawnRotation{};
    DirectX::XMFLOAT3 m_enemySpawnPosition{6.0f, 0.5f, 4.0f};
    DirectX::XMFLOAT3 m_enemySpawnRotation{};
    DirectX::XMFLOAT3 m_pickupPosition{-4.0f, 0.35f, 3.0f};
    DirectX::XMFLOAT3 m_cameraSpawnPosition{0.0f, 20.0f, 0.0f};
    float m_cameraPanX = 0.0f;
    float m_cameraPanZ = 0.0f;
    float m_cameraPitchDegrees = 89.0f;
    float m_cameraYawDegrees = 0.0f;
    float m_playerYawDegrees = 0.0f;
    float m_enemyYawDegrees = 0.0f;
    float m_enemyAttackCooldown = 0.0f;
    float m_playerHitFlashRemaining = 0.0f;
    float m_enemyHitFlashRemaining = 0.0f;
    float m_pickupTime = 0.0f;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_enemyEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_pickupEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_hudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_enemyHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_pickupHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_attackHeld = false;
    bool m_collectHeld = false;
    bool m_restartHeld = false;
};
