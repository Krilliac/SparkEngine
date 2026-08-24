#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

enum class RPGStarterQuestStage : uint8_t
{
    NotStarted,
    FindRelic,
    DefeatWarden,
    ReturnToElder,
    Complete
};

struct RPGStarterState
{
    float x = 0.0f;
    float z = 0.0f;
    float health = 100.0f;
    float enemyHealth = 60.0f;
    uint32_t gold = 0;
    uint32_t experience = 0;
    RPGStarterQuestStage questStage = RPGStarterQuestStage::NotStarted;
    bool dialogueOpen = false;
    bool enemyDefeated = false;
};

class RPGStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "RPGStarter";
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
        m_savedState = {};
        m_savedInventory.clear();
        m_savedRewardClaimed = false;
        m_hasSave = false;

        if (!m_runtime.Load(context, "RPGStarter", {"Startup.sparkscene", "Scenes/Village.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_groundEntity = scene.Find("Ground");
                                m_lightEntity = scene.Find("Directional Light");
                                m_cameraEntity = scene.Find("Main Camera");
                                m_heroEntity = scene.Find("Hero");
                                m_elderEntity = scene.Find("Village Elder");
                                m_relicEntity = scene.Find("Lost Relic");
                                m_wardenEntity = scene.Find("Training Warden");
                                m_houseEntities = {scene.Find("Village House A"), scene.Find("Village House B")};

                                return HasVisual(scene, m_groundEntity) && scene.Get<Transform>(m_lightEntity) &&
                                       scene.Get<LightComponent>(m_lightEntity) &&
                                       scene.Get<Transform>(m_cameraEntity) && scene.Get<Camera>(m_cameraEntity) &&
                                       HasVisual(scene, m_heroEntity) && HasVisual(scene, m_elderEntity) &&
                                       HasVisual(scene, m_relicEntity) && HasVisual(scene, m_wardenEntity) &&
                                       HasVisual(scene, m_houseEntities[0]) && HasVisual(scene, m_houseEntities[1]);
                            }))
            return false;

        if (m_runtime.IsActive())
            CaptureAuthoredScene();
        NewGame();
        if (m_runtime.IsActive() && m_runtime.GetGraphics())
        {
            m_statusHudEntity = m_runtime.CreateSprite("RPG Health And XP HUD", "Assets/rpg_runtime_sheet.png",
                                                       Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2));
            m_saveHudEntity = m_runtime.CreateSprite("RPG Save Slot HUD", "Assets/rpg_runtime_sheet.png",
                                                     Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2));
        }
        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetRuntimeHandles();
        m_context = nullptr;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;
        UpdateRuntimeInput(deltaTime);
        UpdateWardenRetaliation(deltaTime);
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    void Move(float xAxis, float zAxis, float deltaTime)
    {
        if (!std::isfinite(xAxis) || !std::isfinite(zAxis) || !std::isfinite(deltaTime) || deltaTime <= 0.0f ||
            m_state.dialogueOpen || m_state.health <= 0.0f)
            return;

        xAxis = std::clamp(xAxis, -1.0f, 1.0f);
        zAxis = std::clamp(zAxis, -1.0f, 1.0f);
        const float length = std::sqrt(xAxis * xAxis + zAxis * zAxis);
        if (length > 1.0f)
        {
            xAxis /= length;
            zAxis /= length;
        }
        m_state.x += xAxis * kMoveSpeed * deltaTime;
        m_state.z += zAxis * kMoveSpeed * deltaTime;
    }

    void TalkToElder()
    {
        m_state.dialogueOpen = true;
        if (m_state.questStage == RPGStarterQuestStage::NotStarted)
            m_state.questStage = RPGStarterQuestStage::FindRelic;
        else if (m_state.questStage == RPGStarterQuestStage::ReturnToElder)
            m_state.questStage = RPGStarterQuestStage::Complete;
    }

    void CloseDialogue() { m_state.dialogueOpen = false; }

    bool PickUpRelic()
    {
        if (m_state.questStage != RPGStarterQuestStage::FindRelic || HasItem("Lost Relic"))
            return false;
        m_inventory.emplace_back("Lost Relic");
        m_state.questStage = RPGStarterQuestStage::DefeatWarden;
        return true;
    }

    bool AttackWarden()
    {
        if (m_state.health <= 0.0f || m_state.questStage != RPGStarterQuestStage::DefeatWarden || m_state.enemyDefeated)
            return false;
        m_state.enemyHealth = std::max(0.0f, m_state.enemyHealth - 20.0f);
        if (m_state.enemyHealth == 0.0f)
        {
            m_state.enemyDefeated = true;
            m_state.questStage = RPGStarterQuestStage::ReturnToElder;
        }
        return true;
    }

    bool ClaimReward()
    {
        if (m_state.questStage != RPGStarterQuestStage::Complete || m_rewardClaimed)
            return false;
        m_state.gold += 50;
        m_state.experience += 100;
        m_rewardClaimed = true;
        return true;
    }

    void SaveToSlot()
    {
        m_savedState = m_state;
        m_savedInventory = m_inventory;
        m_savedRewardClaimed = m_rewardClaimed;
        m_hasSave = true;
    }

    bool LoadFromSlot()
    {
        if (!m_hasSave)
            return false;
        m_state = m_savedState;
        m_inventory = m_savedInventory;
        m_rewardClaimed = m_savedRewardClaimed;
        m_enemyAttackCooldown = 0.0f;
        return true;
    }

    void NewGame()
    {
        m_state = {};
        m_state.x = m_heroSpawn.x;
        m_state.z = m_heroSpawn.z;
        m_inventory.clear();
        m_rewardClaimed = false;
        m_enemyAttackCooldown = 0.0f;
    }

    [[nodiscard]] const RPGStarterState& GetState() const { return m_state; }
    [[nodiscard]] const std::vector<std::string>& GetInventory() const { return m_inventory; }
    [[nodiscard]] bool HasSave() const { return m_hasSave; }
    [[nodiscard]] bool IsRewardClaimed() const { return m_rewardClaimed; }
    [[nodiscard]] bool HasItem(const std::string& item) const
    {
        return std::find(m_inventory.begin(), m_inventory.end(), item) != m_inventory.end();
    }

  private:
    static constexpr float kMoveSpeed = 4.0f;
    static constexpr float kInteractionRadius = 2.25f;
    static constexpr float kCombatRadius = 2.75f;
    static constexpr float kWardenDamage = 10.0f;
    static constexpr float kWardenAttackInterval = 1.0f;

    static bool HasVisual(const Spark::Templates::TemplateRuntimeScene& scene, uint32_t entity)
    {
        return scene.Get<Transform>(entity) && scene.Get<MeshRenderer>(entity);
    }

    void ResetRuntimeHandles()
    {
        constexpr uint32_t invalid = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_groundEntity = invalid;
        m_lightEntity = invalid;
        m_cameraEntity = invalid;
        m_heroEntity = invalid;
        m_elderEntity = invalid;
        m_relicEntity = invalid;
        m_wardenEntity = invalid;
        m_houseEntities.fill(invalid);
        m_statusHudEntity = invalid;
        m_saveHudEntity = invalid;
    }

    void ResetAuthoredDefaults()
    {
        m_heroSpawn = {};
        m_heroHeight = 0.0f;
        m_cameraOffset = {0.0f, 8.0f, -10.0f};
        m_elderPosition = {1000.0f, 0.0f, 1000.0f};
        m_relicPosition = {1000.0f, 0.0f, 1000.0f};
        m_wardenPosition = {1000.0f, 0.0f, 1000.0f};
    }

    void CaptureAuthoredScene()
    {
        const Transform& hero = *m_runtime.Get<Transform>(m_heroEntity);
        const Transform& camera = *m_runtime.Get<Transform>(m_cameraEntity);
        m_heroSpawn = hero.position;
        m_heroHeight = hero.position.y;
        m_cameraOffset = {camera.position.x - hero.position.x, camera.position.y - hero.position.y,
                          camera.position.z - hero.position.z};
        m_elderPosition = m_runtime.Get<Transform>(m_elderEntity)->position;
        m_relicPosition = m_runtime.Get<Transform>(m_relicEntity)->position;
        m_wardenPosition = m_runtime.Get<Transform>(m_wardenEntity)->position;
    }

    [[nodiscard]] bool IsNear(const DirectX::XMFLOAT3& position, float radius) const
    {
        const float dx = m_state.x - position.x;
        const float dz = m_state.z - position.z;
        return dx * dx + dz * dz <= radius * radius;
    }

    void UpdateRuntimeInput(float deltaTime)
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        if (input->WasKeyPressed('R'))
        {
            NewGame();
            return;
        }
        if (input->WasKeyPressed(VK_F5))
            SaveToSlot();
        if (input->WasKeyPressed(VK_F9))
            LoadFromSlot();
        if (input->WasKeyPressed(VK_ESCAPE) && m_state.dialogueOpen)
            CloseDialogue();

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

        if (input->WasKeyPressed('E'))
        {
            if (m_state.dialogueOpen)
                CloseDialogue();
            else if (IsNear(m_elderPosition, kInteractionRadius))
            {
                TalkToElder();
                if (m_state.questStage == RPGStarterQuestStage::Complete)
                    ClaimReward();
            }
            else if (IsNear(m_relicPosition, kInteractionRadius))
                PickUpRelic();
        }

        const bool attackPressed = input->WasKeyPressed(VK_SPACE) || input->WasMouseButtonPressed(0);
        if (attackPressed && !m_state.dialogueOpen && IsNear(m_wardenPosition, kCombatRadius))
            AttackWarden();
    }

    void UpdateWardenRetaliation(float deltaTime)
    {
        m_enemyAttackCooldown = std::max(0.0f, m_enemyAttackCooldown - deltaTime);
        if (m_state.health <= 0.0f || m_state.dialogueOpen || m_state.enemyDefeated ||
            m_state.questStage != RPGStarterQuestStage::DefeatWarden || !IsNear(m_wardenPosition, kCombatRadius) ||
            m_enemyAttackCooldown > 0.0f)
            return;

        m_state.health = std::max(0.0f, m_state.health - kWardenDamage);
        m_enemyAttackCooldown = kWardenAttackInterval;
        if (m_state.health == 0.0f)
            m_state.dialogueOpen = false;
    }

    void SyncRuntimeState()
    {
        if (Transform* hero = m_runtime.Get<Transform>(m_heroEntity))
            hero->position = {m_state.x, m_heroHeight, m_state.z};
        if (MeshRenderer* hero = m_runtime.Get<MeshRenderer>(m_heroEntity))
            hero->visible = m_state.health > 0.0f;
        if (MeshRenderer* relic = m_runtime.Get<MeshRenderer>(m_relicEntity))
            relic->visible = !HasItem("Lost Relic");
        if (MeshRenderer* warden = m_runtime.Get<MeshRenderer>(m_wardenEntity))
            warden->visible = !m_state.enemyDefeated;

        if (Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            camera->position = {m_state.x + m_cameraOffset.x, m_heroHeight + m_cameraOffset.y,
                                m_state.z + m_cameraOffset.z};
        }

        if (SpriteRenderer* status = m_runtime.Get<SpriteRenderer>(m_statusHudEntity))
        {
            const float healthRatio = std::clamp(m_state.health / 100.0f, 0.0f, 1.0f);
            status->color = {1.0f, 0.35f + 0.65f * healthRatio, 0.35f + 0.65f * healthRatio, 1.0f};
        }
        if (SpriteRenderer* save = m_runtime.Get<SpriteRenderer>(m_saveHudEntity))
        {
            save->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(m_hasSave ? 2u : 1u, 2);
        }
        m_runtime.PlaceHud(m_cameraEntity, m_statusHudEntity, -0.12f, 0.08f, 0.32f, 0.055f, 0.055f);
        m_runtime.PlaceHud(m_cameraEntity, m_saveHudEntity, 0.12f, 0.08f, 0.32f, 0.055f, 0.055f);
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    RPGStarterState m_state;
    RPGStarterState m_savedState;
    std::vector<std::string> m_inventory;
    std::vector<std::string> m_savedInventory;
    DirectX::XMFLOAT3 m_heroSpawn{};
    DirectX::XMFLOAT3 m_cameraOffset{};
    DirectX::XMFLOAT3 m_elderPosition{};
    DirectX::XMFLOAT3 m_relicPosition{};
    DirectX::XMFLOAT3 m_wardenPosition{};
    float m_heroHeight = 0.0f;
    float m_enemyAttackCooldown = 0.0f;
    bool m_rewardClaimed = false;
    bool m_savedRewardClaimed = false;
    bool m_hasSave = false;
    uint32_t m_groundEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_lightEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_heroEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_elderEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_relicEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_wardenEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    std::array<uint32_t, 2> m_houseEntities{};
    uint32_t m_statusHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_saveHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
};
