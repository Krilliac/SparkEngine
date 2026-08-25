#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

enum class MMOStarterFaction : uint8_t
{
    Unassigned = 0,
    Azure = 1,
    Ember = 2
};

struct MMOStarterState
{
    std::string characterName;
    MMOStarterFaction faction = MMOStarterFaction::Unassigned;
    float playerHealth = 100.0f;
    float botHealth = 75.0f;
    float captureProgress = 0.0f;
    float respawnRemaining = 0.0f;
    float botRespawnRemaining = 0.0f;
    float spawnProtectionRemaining = 0.0f;
    uint32_t deaths = 0;
    uint32_t botDefeats = 0;
    bool serverRunning = false;
    bool clientConnected = false;
    bool characterCreated = false;
    bool playerAlive = true;
    bool objectiveCaptured = false;
};

class MMOStarterModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "MMOStarter";
        info.version = "0.3.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        ResetSession();
        ResetRuntimeHandles();
        ResetInputEdges();

        if (!m_runtime.Load(context, "MMOStarter", {"Startup.sparkscene", "Scenes/Frontier.sparkscene"},
                            [this](const Spark::Templates::TemplateRuntimeScene& scene)
                            {
                                m_cameraEntity = scene.Find("Main Camera");
                                m_playerEntity = scene.Find("Local Player");
                                m_botEntity = scene.Find("Training Bot");
                                m_captureEntity = scene.Find("Capture Point");
                                return scene.Get<Transform>(m_cameraEntity) && scene.Get<Camera>(m_cameraEntity) &&
                                       scene.Get<Transform>(m_playerEntity) &&
                                       scene.Get<MeshRenderer>(m_playerEntity) && scene.Get<Transform>(m_botEntity) &&
                                       scene.Get<MeshRenderer>(m_botEntity) && scene.Get<Transform>(m_captureEntity) &&
                                       scene.Get<MeshRenderer>(m_captureEntity);
                            }))
            return false;

        if (m_runtime.IsActive())
        {
            RememberSpawnPoints();
            StartPlayableSession();
            if (m_runtime.GetGraphics())
            {
                m_factionHudEntity = m_runtime.CreateSprite("MMO Faction HUD", "Assets/mmo_starter_runtime_sheet.png",
                                                            Spark::Templates::TemplateRuntimeScene::SheetCell(0, 0));
                m_objectiveHudEntity =
                    m_runtime.CreateSprite("MMO Objective HUD", "Assets/mmo_starter_runtime_sheet.png",
                                           Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2));
                m_statusHudEntity = m_runtime.CreateSprite("MMO Status HUD", "Assets/mmo_starter_runtime_sheet.png",
                                                           Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2));
            }
        }

        SyncRuntimeState();
        return true;
    }

    void OnUnload() override
    {
        m_runtime.Unload();
        ResetRuntimeHandles();
        m_context = nullptr;
        m_state.serverRunning = false;
        m_state.clientConnected = false;
    }

    bool SupportsHotReload() const override { return false; }

    void OnUpdate(float deltaTime) override
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        if (!m_state.playerAlive)
        {
            m_state.respawnRemaining = std::max(0.0f, m_state.respawnRemaining - deltaTime);
            if (m_state.respawnRemaining == 0.0f)
            {
                m_state.playerHealth = 100.0f;
                m_state.playerAlive = true;
                m_state.spawnProtectionRemaining = SpawnProtectionSeconds;
                ResetPlayerTransform();
            }
        }
        else
        {
            m_state.spawnProtectionRemaining = std::max(0.0f, m_state.spawnProtectionRemaining - deltaTime);
        }

        UpdateRuntimeInput(deltaTime);
        UpdateTrainingBot(deltaTime);
        SyncRuntimeState();
    }

    void OnRender() override { m_runtime.Render(m_cameraEntity); }
    void OnResize(int width, int height) override { m_runtime.Resize(width, height); }

    bool StartLocalSession()
    {
        if (m_state.serverRunning)
            return false;
        m_state.serverRunning = true;
        m_state.clientConnected = true;
        return true;
    }

    bool CreateCharacter(const std::string& name)
    {
        if (!m_state.clientConnected || name.size() < 3 || name.size() > 16 || !HasVisibleText(name))
            return false;
        m_state.characterName = name;
        m_state.characterCreated = true;
        return true;
    }

    bool SelectFaction(MMOStarterFaction faction)
    {
        if (!m_state.characterCreated || (faction != MMOStarterFaction::Azure && faction != MMOStarterFaction::Ember))
            return false;
        m_state.faction = faction;
        return true;
    }

    bool AdvanceCapture(float seconds)
    {
        if (!CanPlay() || !std::isfinite(seconds) || seconds <= 0.0f || m_state.objectiveCaptured)
            return false;
        m_state.captureProgress = std::min(100.0f, m_state.captureProgress + seconds * 20.0f);
        m_state.objectiveCaptured = m_state.captureProgress == 100.0f;
        return true;
    }

    bool AttackBot()
    {
        if (!CanPlay() || m_state.botHealth <= 0.0f)
            return false;
        m_state.botHealth = std::max(0.0f, m_state.botHealth - 25.0f);
        if (m_state.botHealth == 0.0f)
        {
            ++m_state.botDefeats;
            m_state.botRespawnRemaining = BotRespawnSeconds;
        }
        return true;
    }

    void DamagePlayer(float amount)
    {
        if (!CanPlay() || m_state.spawnProtectionRemaining > 0.0f || !std::isfinite(amount) || amount <= 0.0f)
            return;
        m_state.playerHealth = std::max(0.0f, m_state.playerHealth - amount);
        if (m_state.playerHealth == 0.0f)
        {
            m_state.playerAlive = false;
            m_state.respawnRemaining = 3.0f;
            ++m_state.deaths;
        }
    }

    bool SubmitChat(const std::string& message)
    {
        if (!m_state.clientConnected || !m_state.characterCreated || message.empty() || message.size() > 120 ||
            !HasVisibleText(message))
            return false;
        if (m_chatLog.size() == 8)
            m_chatLog.erase(m_chatLog.begin());
        m_chatLog.push_back(m_state.characterName.empty() ? message : m_state.characterName + ": " + message);
        return true;
    }

    void ResetSession()
    {
        m_state = {};
        m_chatLog.clear();
    }

    [[nodiscard]] const MMOStarterState& GetState() const { return m_state; }
    [[nodiscard]] const std::vector<std::string>& GetChatLog() const { return m_chatLog; }
    [[nodiscard]] bool CanPlay() const
    {
        const bool hasPlayableFaction =
            m_state.faction == MMOStarterFaction::Azure || m_state.faction == MMOStarterFaction::Ember;
        return m_state.serverRunning && m_state.clientConnected && m_state.characterCreated && hasPlayableFaction &&
               m_state.playerAlive;
    }

  private:
    static constexpr float PlayerMoveSpeed = 6.0f;
    static constexpr float BotMoveSpeed = 2.5f;
    static constexpr float ArenaLimit = 22.0f;
    static constexpr float BotAttackRangeSquared = 12.25f;
    static constexpr float BotMeleeRangeSquared = 4.0f;
    static constexpr float CaptureRangeSquared = 25.0f;
    static constexpr float BotAttackInterval = 1.2f;
    static constexpr float BotDamage = 12.0f;
    static constexpr float BotRespawnSeconds = 4.0f;
    static constexpr float SpawnProtectionSeconds = 1.5f;

    [[nodiscard]] static bool HasVisibleText(const std::string& value)
    {
        return std::ranges::any_of(value, [](unsigned char character) { return !std::isspace(character); });
    }

    void ResetRuntimeHandles()
    {
        m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_botEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_captureEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_factionHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_objectiveHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_statusHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    }

    void ResetInputEdges()
    {
        m_attackHeld = false;
        m_damageHeld = false;
        m_chatHeld = false;
        m_resetHeld = false;
    }

    void RememberSpawnPoints()
    {
        if (const Transform* player = m_runtime.Get<Transform>(m_playerEntity))
        {
            m_playerSpawn = player->position;
            m_playerSpawnYawDegrees = player->rotation.y;
        }
        if (const Transform* bot = m_runtime.Get<Transform>(m_botEntity))
        {
            m_botSpawn = bot->position;
            m_botSpawnYawDegrees = bot->rotation.y;
        }
        if (const Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            m_cameraOffset = {camera->position.x - m_playerSpawn.x, camera->position.y - m_playerSpawn.y,
                              camera->position.z - m_playerSpawn.z};
        }
    }

    void StartPlayableSession()
    {
        ResetSession();
        StartLocalSession();
        CreateCharacter("Astra");
        SelectFaction(MMOStarterFaction::Azure);
        m_botAttackCooldown = 0.0f;
        m_playerYawDegrees = m_playerSpawnYawDegrees;
        m_botYawDegrees = m_botSpawnYawDegrees;
        m_state.spawnProtectionRemaining = SpawnProtectionSeconds;
        ResetPlayerTransform();
        if (Transform* bot = m_runtime.Get<Transform>(m_botEntity))
        {
            bot->position = m_botSpawn;
            bot->rotation.y = m_botYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_botEntity))
                mesh->worldMatrixDirty = true;
        }
    }

    void ResetPlayerTransform()
    {
        m_playerYawDegrees = m_playerSpawnYawDegrees;
        if (Transform* player = m_runtime.Get<Transform>(m_playerEntity))
        {
            player->position = m_playerSpawn;
            player->rotation.y = m_playerYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_playerEntity))
                mesh->worldMatrixDirty = true;
        }
    }

    [[nodiscard]] bool IsWithin(uint32_t firstEntity, uint32_t secondEntity, float rangeSquared) const
    {
        const Transform* first = m_runtime.Get<Transform>(firstEntity);
        const Transform* second = m_runtime.Get<Transform>(secondEntity);
        if (!first || !second)
            return false;
        const float x = first->position.x - second->position.x;
        const float z = first->position.z - second->position.z;
        return x * x + z * z <= rangeSquared;
    }

    void UpdateRuntimeInput(float deltaTime)
    {
        InputManager* input = m_runtime.GetInput();
        if (!input)
            return;

        const bool resetDown = input->IsKeyDown('R');
        if (resetDown && !m_resetHeld)
        {
            StartPlayableSession();
            ResetInputEdges();
            m_resetHeld = true;
            return;
        }
        m_resetHeld = resetDown;

        if (input->IsKeyDown('1'))
            SelectFaction(MMOStarterFaction::Azure);
        if (input->IsKeyDown('2'))
            SelectFaction(MMOStarterFaction::Ember);

        if (CanPlay())
        {
            float x = 0.0f;
            float z = 0.0f;
            if (input->IsKeyDown('W'))
                z += 1.0f;
            if (input->IsKeyDown('S'))
                z -= 1.0f;
            if (input->IsKeyDown('D'))
                x += 1.0f;
            if (input->IsKeyDown('A'))
                x -= 1.0f;
            const float length = std::sqrt(x * x + z * z);
            if (length > 1.0f)
            {
                x /= length;
                z /= length;
            }
            if (Transform* player = m_runtime.Get<Transform>(m_playerEntity))
            {
                if (std::abs(x) > 0.0001f || std::abs(z) > 0.0001f)
                    m_playerYawDegrees = std::atan2(x, z) * RadiansToDegrees;
                player->position.x =
                    std::clamp(player->position.x + x * PlayerMoveSpeed * deltaTime, -ArenaLimit, ArenaLimit);
                player->position.z =
                    std::clamp(player->position.z + z * PlayerMoveSpeed * deltaTime, -ArenaLimit, ArenaLimit);
                player->rotation.y = m_playerYawDegrees;
                if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_playerEntity))
                    mesh->worldMatrixDirty = true;
            }

            if (input->IsKeyDown('E') && IsWithin(m_playerEntity, m_captureEntity, CaptureRangeSquared))
                AdvanceCapture(deltaTime);
        }

        const bool attackDown = input->IsKeyDown(' ');
        if (attackDown && !m_attackHeld && IsWithin(m_playerEntity, m_botEntity, BotAttackRangeSquared))
            AttackBot();
        m_attackHeld = attackDown;

        const bool damageDown = input->IsKeyDown('H');
        if (damageDown && !m_damageHeld)
            DamagePlayer(25.0f);
        m_damageHeld = damageDown;

        const bool chatDown = input->IsKeyDown('T');
        if (chatDown && !m_chatHeld)
            SubmitChat("Frontier secure.");
        m_chatHeld = chatDown;
    }

    void UpdateTrainingBot(float deltaTime)
    {
        m_botAttackCooldown = std::max(0.0f, m_botAttackCooldown - deltaTime);
        if (m_state.botHealth <= 0.0f)
        {
            if (m_state.objectiveCaptured)
                return;
            m_state.botRespawnRemaining = std::max(0.0f, m_state.botRespawnRemaining - deltaTime);
            if (m_state.botRespawnRemaining > 0.0f)
                return;
            m_state.botHealth = 75.0f;
            m_botYawDegrees = m_botSpawnYawDegrees;
            if (Transform* bot = m_runtime.Get<Transform>(m_botEntity))
            {
                bot->position = m_botSpawn;
                bot->rotation.y = m_botYawDegrees;
                if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_botEntity))
                    mesh->worldMatrixDirty = true;
            }
        }

        if (!CanPlay() || m_state.objectiveCaptured)
            return;

        Transform* bot = m_runtime.Get<Transform>(m_botEntity);
        const Transform* player = m_runtime.Get<Transform>(m_playerEntity);
        if (!bot || !player)
            return;

        const float dx = player->position.x - bot->position.x;
        const float dz = player->position.z - bot->position.z;
        const float distanceSquared = dx * dx + dz * dz;
        if (distanceSquared > BotMeleeRangeSquared)
        {
            const float distance = std::sqrt(distanceSquared);
            const float step = std::min(distance, BotMoveSpeed * deltaTime);
            bot->position.x = std::clamp(bot->position.x + dx / distance * step, -ArenaLimit, ArenaLimit);
            bot->position.z = std::clamp(bot->position.z + dz / distance * step, -ArenaLimit, ArenaLimit);
            m_botYawDegrees = std::atan2(dx, dz) * RadiansToDegrees;
            bot->rotation.y = m_botYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_botEntity))
                mesh->worldMatrixDirty = true;
        }
        else if (m_botAttackCooldown == 0.0f && m_state.spawnProtectionRemaining == 0.0f)
        {
            DamagePlayer(BotDamage);
            m_botAttackCooldown = BotAttackInterval;
        }
    }

    void SyncRuntimeState()
    {
        if (MeshRenderer* player = m_runtime.Get<MeshRenderer>(m_playerEntity))
        {
            player->visible = m_state.playerAlive;
            player->emissive = m_state.faction == MMOStarterFaction::Ember ? 0.2f : 0.08f;
        }
        if (MeshRenderer* bot = m_runtime.Get<MeshRenderer>(m_botEntity))
        {
            bot->visible = m_state.botHealth > 0.0f;
            bot->emissive = 0.05f;
        }
        if (MeshRenderer* capture = m_runtime.Get<MeshRenderer>(m_captureEntity))
            capture->emissive = m_state.objectiveCaptured ? 0.8f : m_state.captureProgress / 250.0f;

        if (Transform* camera = m_runtime.Get<Transform>(m_cameraEntity))
        {
            if (const Transform* player = m_runtime.Get<Transform>(m_playerEntity))
            {
                camera->position = {player->position.x + m_cameraOffset.x, player->position.y + m_cameraOffset.y,
                                    player->position.z + m_cameraOffset.z};
            }
        }

        if (SpriteRenderer* faction = m_runtime.Get<SpriteRenderer>(m_factionHudEntity))
        {
            const uint32_t column = m_state.faction == MMOStarterFaction::Ember ? 1u : 0u;
            faction->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(column, 0);
            faction->visible = m_state.characterCreated;
        }
        if (SpriteRenderer* objective = m_runtime.Get<SpriteRenderer>(m_objectiveHudEntity))
        {
            objective->sourceRect =
                Spark::Templates::TemplateRuntimeScene::SheetCell(m_state.objectiveCaptured ? 2 : 1, 2);
            objective->visible = CanPlay() || m_state.objectiveCaptured;
        }
        if (SpriteRenderer* status = m_runtime.Get<SpriteRenderer>(m_statusHudEntity))
        {
            if (!m_state.playerAlive)
                status->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 1);
            else if (m_state.spawnProtectionRemaining > 0.0f || m_state.botHealth == 0.0f)
                status->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 1);
            else if (!m_chatLog.empty())
                status->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2);
            else
                status->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 1);
            status->visible = !m_state.playerAlive || m_state.spawnProtectionRemaining > 0.0f ||
                              m_state.botHealth < 75.0f || !m_chatLog.empty();
        }

        m_runtime.PlaceHud(m_cameraEntity, m_factionHudEntity, -0.13f, 0.08f, 0.32f, 0.05f, 0.05f);
        m_runtime.PlaceHud(m_cameraEntity, m_objectiveHudEntity, 0.0f, 0.08f, 0.32f, 0.05f, 0.05f);
        m_runtime.PlaceHud(m_cameraEntity, m_statusHudEntity, 0.13f, 0.08f, 0.32f, 0.05f, 0.05f);
    }

    Spark::IEngineContext* m_context = nullptr;
    Spark::Templates::TemplateRuntimeScene m_runtime;
    MMOStarterState m_state;
    std::vector<std::string> m_chatLog;
    DirectX::XMFLOAT3 m_playerSpawn{};
    DirectX::XMFLOAT3 m_botSpawn{};
    DirectX::XMFLOAT3 m_cameraOffset{0.0f, 7.0f, -12.0f};
    float m_playerYawDegrees = 0.0f;
    float m_botYawDegrees = 0.0f;
    float m_playerSpawnYawDegrees = 0.0f;
    float m_botSpawnYawDegrees = 0.0f;
    float m_botAttackCooldown = 0.0f;
    static constexpr float RadiansToDegrees = 57.29577951308232f;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_playerEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_botEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_captureEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_factionHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_objectiveHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_statusHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    bool m_attackHeld = false;
    bool m_damageHeld = false;
    bool m_chatHeld = false;
    bool m_resetHeld = false;
};
