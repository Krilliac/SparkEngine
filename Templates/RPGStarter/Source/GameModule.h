#pragma once

#include <Spark/SparkSDK.h>

#include "Game/TemplateRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        info.version = "0.3.0";
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
        m_hasDiskSave = false;

        if (!m_runtime.Load(
                context, "RPGStarter", {"Startup.sparkscene", "Scenes/Village.sparkscene"},
                [this](const Spark::Templates::TemplateRuntimeScene& scene)
                {
                    m_cameraEntity = scene.Find("Main Camera");
                    m_heroEntity = scene.Find("Hero");
                    m_elderEntity = scene.Find("Village Elder");
                    m_relicEntity = scene.Find("Lost Relic");
                    m_wardenEntity = scene.Find("Training Warden");

                    // Required: the ground, the camera, and every entity the quest
                    // logic drives. Nothing outside this contract reads the ground,
                    // the light or the houses, so they stay local: the ground is a
                    // hard requirement, the decoration only warns.
                    if (!HasVisual(scene, scene.Find("Ground")) || !scene.Get<Transform>(m_cameraEntity) ||
                        !scene.Get<Camera>(m_cameraEntity) || !HasVisual(scene, m_heroEntity) ||
                        !HasVisual(scene, m_elderEntity) || !HasVisual(scene, m_relicEntity) ||
                        !HasVisual(scene, m_wardenEntity))
                        return false;
                    const uint32_t light = scene.Find("Directional Light");
                    if (!scene.Get<Transform>(light) || !scene.Get<LightComponent>(light))
                        SPARK_LOG_WARN(Spark::LogCategory::Game, "RPGStarter scene has no usable 'Directional Light'");
                    for (const char* house : {"Village House A", "Village House B"})
                    {
                        if (!HasVisual(scene, scene.Find(house)))
                            SPARK_LOG_WARN(Spark::LogCategory::Game,
                                           "RPGStarter scene is missing the village house prop '%s'", house);
                    }
                    return true;
                }))
            return false;

        if (m_runtime.IsActive())
            CaptureAuthoredScene();
        NewGame();
        // A slot written by a previous session is what the HUD must show on
        // startup, so probe the disk once here instead of stat-ing every frame.
        m_hasDiskSave = SaveFileExists();
        if (m_runtime.IsActive() && m_runtime.GetGraphics())
        {
            m_statusHudEntity = m_runtime.CreateSprite("RPG Health And XP HUD", "Assets/rpg_runtime_sheet.png",
                                                       Spark::Templates::TemplateRuntimeScene::SheetCell(0, 2));
            m_saveHudEntity = m_runtime.CreateSprite("RPG Save Slot HUD", "Assets/rpg_runtime_sheet.png",
                                                     Spark::Templates::TemplateRuntimeScene::SheetCell(1, 2));
            m_questHudEntity = m_runtime.CreateSprite("RPG Quest Objective HUD", "Assets/rpg_runtime_sheet.png",
                                                      Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0));
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
        m_state.x = std::clamp(m_state.x + xAxis * kMoveSpeed * deltaTime, -kVillageLimit, kVillageLimit);
        m_state.z = std::clamp(m_state.z + zAxis * kMoveSpeed * deltaTime, -kVillageLimit, kVillageLimit);
        if (std::abs(xAxis) > 0.0001f || std::abs(zAxis) > 0.0001f)
            m_heroYawDegrees = std::atan2(xAxis, zAxis) * kRadiansToDegrees;
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

    /**
     * Snapshot the run and persist it to `Saves/rpg_slot0.spark_save` under the
     * project root so a save survives the process. Returns false when only the
     * in-memory snapshot could be taken (no project root, or the write failed);
     * the slot HUD then keeps reporting "no save on disk", because there is none.
     */
    bool SaveToSlot()
    {
        m_savedState = m_state;
        m_savedInventory = m_inventory;
        m_savedRewardClaimed = m_rewardClaimed;
        m_savedHeroYawDegrees = m_heroYawDegrees;
        m_savedWardenYawDegrees = m_wardenYawDegrees;
        m_savedEnemyAttackCooldown = m_enemyAttackCooldown;
        m_hasSave = true;
        m_hasDiskSave = WriteSaveFile();
        return m_hasDiskSave;
    }

    /** Restore the in-memory snapshot, or the on-disk slot when this process has none. */
    bool LoadFromSlot()
    {
        if (!m_hasSave && !ReadSaveFile())
            return false;
        m_state = m_savedState;
        m_inventory = m_savedInventory;
        m_rewardClaimed = m_savedRewardClaimed;
        m_heroYawDegrees = m_savedHeroYawDegrees;
        m_wardenYawDegrees = m_savedWardenYawDegrees;
        m_enemyAttackCooldown = m_savedEnemyAttackCooldown;
        return true;
    }

    /** Project-relative slot path; empty when no project root has been resolved. */
    [[nodiscard]] std::filesystem::path GetSaveFilePath() const
    {
        const std::filesystem::path& root = m_runtime.GetProjectRoot();
        return root.empty() ? std::filesystem::path() : root / "Saves" / "rpg_slot0.spark_save";
    }

    void NewGame()
    {
        m_state = {};
        m_state.x = m_heroSpawn.x;
        m_state.z = m_heroSpawn.z;
        m_inventory.clear();
        m_rewardClaimed = false;
        m_heroYawDegrees = m_heroSpawnYawDegrees;
        m_wardenYawDegrees = m_wardenSpawnYawDegrees;
        m_enemyAttackCooldown = 0.0f;
    }

    [[nodiscard]] const RPGStarterState& GetState() const { return m_state; }
    [[nodiscard]] const std::vector<std::string>& GetInventory() const { return m_inventory; }
    [[nodiscard]] bool HasSave() const { return m_hasSave || SaveFileExists(); }
    /** True only for a slot that actually reached disk; this is what the save HUD reports. */
    [[nodiscard]] bool HasDiskSave() const { return m_hasDiskSave; }
    [[nodiscard]] bool IsRewardClaimed() const { return m_rewardClaimed; }
    [[nodiscard]] float GetEnemyAttackCooldown() const { return m_enemyAttackCooldown; }
    [[nodiscard]] float GetHeroYawDegrees() const { return m_heroYawDegrees; }
    [[nodiscard]] float GetWardenYawDegrees() const { return m_wardenYawDegrees; }
    [[nodiscard]] bool HasItem(const std::string& item) const
    {
        return std::find(m_inventory.begin(), m_inventory.end(), item) != m_inventory.end();
    }

  private:
    static constexpr float kMoveSpeed = 4.0f;
    static constexpr float kVillageLimit = 22.0f;
    static constexpr float kInteractionRadius = 2.25f;
    static constexpr float kCombatRadius = 2.75f;
    static constexpr float kWardenDamage = 10.0f;
    static constexpr float kWardenAttackInterval = 1.0f;
    static constexpr float kRadiansToDegrees = 57.29577951308232f;

    static bool HasVisual(const Spark::Templates::TemplateRuntimeScene& scene, uint32_t entity)
    {
        return scene.Get<Transform>(entity) && scene.Get<MeshRenderer>(entity);
    }

    [[nodiscard]] bool SaveFileExists() const
    {
        const std::filesystem::path path = GetSaveFilePath();
        std::error_code ec;
        return !path.empty() && std::filesystem::is_regular_file(path, ec) && !ec;
    }

    bool WriteSaveFile() const
    {
        const std::filesystem::path path = GetSaveFilePath();
        if (path.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "RPGStarter could not create its save directory");
            return false;
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "RPGStarter could not open its save slot for writing");
            return false;
        }
        file.precision(9);
        file << "version=1\n"
             << "x=" << m_savedState.x << "\n"
             << "z=" << m_savedState.z << "\n"
             << "health=" << m_savedState.health << "\n"
             << "enemyHealth=" << m_savedState.enemyHealth << "\n"
             << "gold=" << m_savedState.gold << "\n"
             << "experience=" << m_savedState.experience << "\n"
             << "questStage=" << static_cast<unsigned>(m_savedState.questStage) << "\n"
             << "dialogueOpen=" << (m_savedState.dialogueOpen ? 1 : 0) << "\n"
             << "enemyDefeated=" << (m_savedState.enemyDefeated ? 1 : 0) << "\n"
             << "rewardClaimed=" << (m_savedRewardClaimed ? 1 : 0) << "\n"
             << "heroYaw=" << m_savedHeroYawDegrees << "\n"
             << "wardenYaw=" << m_savedWardenYawDegrees << "\n"
             << "enemyAttackCooldown=" << m_savedEnemyAttackCooldown << "\n";
        for (const std::string& item : m_savedInventory)
            file << "item=" << item << "\n";
        file.flush();
        return file.good();
    }

    bool ReadSaveFile()
    {
        const std::filesystem::path path = GetSaveFilePath();
        if (path.empty())
            return false;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        RPGStarterState state{};
        std::vector<std::string> inventory;
        float heroYaw = m_heroSpawnYawDegrees;
        float wardenYaw = m_wardenSpawnYawDegrees;
        float cooldown = 0.0f;
        bool rewardClaimed = false;
        unsigned long version = 0;
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const std::size_t split = line.find('=');
            if (split == std::string::npos)
                continue;
            const std::string key = line.substr(0, split);
            const std::string value = line.substr(split + 1);
            if (key == "version")
                version = ParseUnsigned(value);
            else if (key == "x")
                state.x = ParseFloat(value);
            else if (key == "z")
                state.z = ParseFloat(value);
            else if (key == "health")
                state.health = ParseFloat(value);
            else if (key == "enemyHealth")
                state.enemyHealth = ParseFloat(value);
            else if (key == "gold")
                state.gold = static_cast<uint32_t>(ParseUnsigned(value));
            else if (key == "experience")
                state.experience = static_cast<uint32_t>(ParseUnsigned(value));
            else if (key == "questStage")
                state.questStage = static_cast<RPGStarterQuestStage>(std::min<unsigned long>(
                    ParseUnsigned(value), static_cast<unsigned long>(RPGStarterQuestStage::Complete)));
            else if (key == "dialogueOpen")
                state.dialogueOpen = ParseUnsigned(value) != 0;
            else if (key == "enemyDefeated")
                state.enemyDefeated = ParseUnsigned(value) != 0;
            else if (key == "rewardClaimed")
                rewardClaimed = ParseUnsigned(value) != 0;
            else if (key == "heroYaw")
                heroYaw = ParseFloat(value);
            else if (key == "wardenYaw")
                wardenYaw = ParseFloat(value);
            else if (key == "enemyAttackCooldown")
                cooldown = ParseFloat(value);
            else if (key == "item")
                inventory.push_back(value);
        }
        // Every float here came from strtof over untrusted file text: a hand-edited
        // or truncated slot must be rejected, not copied into live state.
        if (version != 1 || !std::isfinite(state.x) || !std::isfinite(state.z) || !std::isfinite(state.health) ||
            !std::isfinite(state.enemyHealth) || !std::isfinite(heroYaw) || !std::isfinite(wardenYaw) ||
            !std::isfinite(cooldown))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "RPGStarter ignored an unreadable save slot");
            return false;
        }
        state.health = std::max(0.0f, state.health);
        state.enemyHealth = std::max(0.0f, state.enemyHealth);
        cooldown = std::max(0.0f, cooldown);

        m_savedState = state;
        m_savedInventory = std::move(inventory);
        m_savedRewardClaimed = rewardClaimed;
        m_savedHeroYawDegrees = heroYaw;
        m_savedWardenYawDegrees = wardenYaw;
        m_savedEnemyAttackCooldown = cooldown;
        m_hasSave = true;
        m_hasDiskSave = true;
        return true;
    }

    static float ParseFloat(const std::string& value) { return std::strtof(value.c_str(), nullptr); }
    static unsigned long ParseUnsigned(const std::string& value) { return std::strtoul(value.c_str(), nullptr, 10); }

    void ResetRuntimeHandles()
    {
        constexpr uint32_t invalid = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
        m_cameraEntity = invalid;
        m_heroEntity = invalid;
        m_elderEntity = invalid;
        m_relicEntity = invalid;
        m_wardenEntity = invalid;
        m_statusHudEntity = invalid;
        m_saveHudEntity = invalid;
        m_questHudEntity = invalid;
    }

    void ResetAuthoredDefaults()
    {
        m_heroSpawn = {};
        m_heroHeight = 0.0f;
        m_cameraOffset = {0.0f, 8.0f, -10.0f};
        m_elderPosition = {1000.0f, 0.0f, 1000.0f};
        m_relicPosition = {1000.0f, 0.0f, 1000.0f};
        m_wardenPosition = {1000.0f, 0.0f, 1000.0f};
        m_heroSpawnYawDegrees = 0.0f;
        m_wardenSpawnYawDegrees = 0.0f;
    }

    void CaptureAuthoredScene()
    {
        const Transform& hero = *m_runtime.Get<Transform>(m_heroEntity);
        const Transform& camera = *m_runtime.Get<Transform>(m_cameraEntity);
        m_heroSpawn = hero.position;
        m_heroHeight = hero.position.y;
        m_heroSpawnYawDegrees = hero.rotation.y;
        m_cameraOffset = {camera.position.x - hero.position.x, camera.position.y - hero.position.y,
                          camera.position.z - hero.position.z};
        m_elderPosition = m_runtime.Get<Transform>(m_elderEntity)->position;
        m_relicPosition = m_runtime.Get<Transform>(m_relicEntity)->position;
        const Transform& warden = *m_runtime.Get<Transform>(m_wardenEntity);
        m_wardenPosition = warden.position;
        m_wardenSpawnYawDegrees = warden.rotation.y;
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
        if (input->WasKeyPressed(VK_F5) && !SaveToSlot())
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "RPGStarter kept the run in memory: the save slot did not reach disk");
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
        if (!m_state.enemyDefeated && m_state.questStage == RPGStarterQuestStage::DefeatWarden)
        {
            const float dx = m_state.x - m_wardenPosition.x;
            const float dz = m_state.z - m_wardenPosition.z;
            if (dx * dx + dz * dz > 0.0001f)
                m_wardenYawDegrees = std::atan2(dx, dz) * kRadiansToDegrees;
        }
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
        {
            hero->position = {m_state.x, m_heroHeight, m_state.z};
            hero->rotation.y = m_heroYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_heroEntity))
                mesh->worldMatrixDirty = true;
        }
        if (MeshRenderer* hero = m_runtime.Get<MeshRenderer>(m_heroEntity))
            hero->visible = m_state.health > 0.0f;
        if (MeshRenderer* relic = m_runtime.Get<MeshRenderer>(m_relicEntity))
            relic->visible = !HasItem("Lost Relic");
        if (MeshRenderer* warden = m_runtime.Get<MeshRenderer>(m_wardenEntity))
            warden->visible = !m_state.enemyDefeated;
        if (Transform* warden = m_runtime.Get<Transform>(m_wardenEntity))
        {
            warden->rotation.y = m_wardenYawDegrees;
            if (MeshRenderer* mesh = m_runtime.Get<MeshRenderer>(m_wardenEntity))
                mesh->worldMatrixDirty = true;
        }

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
            save->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(m_hasDiskSave ? 2u : 1u, 2);
        }
        if (SpriteRenderer* quest = m_runtime.Get<SpriteRenderer>(m_questHudEntity))
        {
            switch (m_state.questStage)
            {
            case RPGStarterQuestStage::NotStarted:
                quest->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 0);
                break;
            case RPGStarterQuestStage::FindRelic:
                quest->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(0, 1);
                break;
            case RPGStarterQuestStage::DefeatWarden:
                quest->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 0);
                break;
            case RPGStarterQuestStage::ReturnToElder:
                quest->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(1, 1);
                break;
            case RPGStarterQuestStage::Complete:
                quest->sourceRect = Spark::Templates::TemplateRuntimeScene::SheetCell(2, 2);
                break;
            }
            quest->color = m_rewardClaimed ? DirectX::XMFLOAT4{0.45f, 1.0f, 0.55f, 1.0f}
                                           : DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f};
        }
        m_runtime.PlaceHud(m_cameraEntity, m_statusHudEntity, -0.12f, 0.08f, 0.32f, 0.055f, 0.055f);
        m_runtime.PlaceHud(m_cameraEntity, m_questHudEntity, 0.0f, 0.08f, 0.32f, 0.055f, 0.055f);
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
    float m_heroYawDegrees = 0.0f;
    float m_heroSpawnYawDegrees = 0.0f;
    float m_savedHeroYawDegrees = 0.0f;
    float m_wardenYawDegrees = 0.0f;
    float m_wardenSpawnYawDegrees = 0.0f;
    float m_savedWardenYawDegrees = 0.0f;
    float m_savedEnemyAttackCooldown = 0.0f;
    bool m_rewardClaimed = false;
    bool m_savedRewardClaimed = false;
    bool m_hasSave = false;
    bool m_hasDiskSave = false;
    uint32_t m_cameraEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_heroEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_elderEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_relicEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_wardenEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_statusHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_saveHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
    uint32_t m_questHudEntity = Spark::Templates::TemplateRuntimeScene::InvalidEntity;
};
