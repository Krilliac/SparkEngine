/**
 * @file MMOGameplaySession.cpp
 * @brief Playable vertical slice for the SparkGameMMO showcase.
 */

#include "MMOGameplaySession.h"

#include "Persistence/MMOPersistenceSystem.h"
#include "Player/MMOPlayerSystem.h"
#include "World/MMOWorldSetup.h"
#include "WorldBoss/MMOWorldBossSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

namespace MMO
{

    bool MMOGameplaySession::Initialize(MMOPlayerSystem* playerSystem, MMOWorldSetup* worldSetup,
                                        MMOInventorySystem* inventorySystem, MMOCraftingSystem* craftingSystem,
                                        MMOAchievementSystem* achievementSystem, MMOReputationSystem* reputationSystem,
                                        MMODungeonSystem* dungeonSystem, MMOWorldBossSystem* worldBossSystem)
    {
        if (!playerSystem || !worldSetup || !inventorySystem || !craftingSystem || !achievementSystem ||
            !reputationSystem || !dungeonSystem || !worldBossSystem)
        {
            return false;
        }

        m_playerSystem = playerSystem;
        m_worldSetup = worldSetup;
        m_inventorySystem = inventorySystem;
        m_craftingSystem = craftingSystem;
        m_achievementSystem = achievementSystem;
        m_reputationSystem = reputationSystem;
        m_dungeonSystem = dungeonSystem;
        m_worldBossSystem = worldBossSystem;
        ResetDemo();
        return true;
    }

    void MMOGameplaySession::Shutdown()
    {
        m_active = false;
        m_inventory = {};
        m_crafting = {};
        m_achievements = {};
        m_reputation = {};
        m_dungeonState = {};
        m_playTime = 0.0f;
        m_respawnTimer = 0.0f;
        m_lastPlayerX = 0.0f;
        m_lastPlayerZ = 0.0f;
        m_distanceTraveled = 0.0f;
        m_areaVisitCountFloor = 0;
        m_visitedAreas.clear();
        m_lastEvent.clear();
        m_playerSystem = nullptr;
        m_worldSetup = nullptr;
        m_inventorySystem = nullptr;
        m_craftingSystem = nullptr;
        m_achievementSystem = nullptr;
        m_reputationSystem = nullptr;
        m_dungeonSystem = nullptr;
        m_worldBossSystem = nullptr;
    }

    void MMOGameplaySession::InitializePlayerProgress()
    {
        m_inventory = {};
        m_inventory.currency = 100;
        m_crafting = {};
        m_crafting.nearbyStation = CraftingStation::AlchemyLab;
        m_crafting.skills[static_cast<int>(CraftingDiscipline::Alchemy)] = {CraftingDiscipline::Alchemy, 1, 0, 100};
        m_crafting.skills[static_cast<int>(CraftingDiscipline::Weaponsmithing)] = {CraftingDiscipline::Weaponsmithing,
                                                                                   1, 0, 100};
        m_craftingSystem->LearnRecipe(m_crafting, 100);
        m_craftingSystem->LearnRecipe(m_crafting, 200);

        // A small deterministic starter kit makes the first gather/craft/use loop
        // playable without depending on random loot.
        m_inventorySystem->AddItem(m_inventory, 1, 1);
        m_inventorySystem->AddItem(m_inventory, 10, 2);
        m_inventorySystem->AddItem(m_inventory, 11, 1);
        m_inventorySystem->AddItem(m_inventory, 13, 3);
        m_inventorySystem->AddItem(m_inventory, 14, 1);

        m_achievements = {};
        m_achievementSystem->InitializePlayerState(m_achievements);
        m_reputation = {};
        m_reputationSystem->InitializePlayerRep(m_reputation);
        m_dungeonState = {};
        m_playTime = 0.0f;
        m_respawnTimer = 0.0f;
        m_distanceTraveled = 0.0f;
        m_areaVisitCountFloor = 0;
        m_visitedAreas.clear();
        if (const auto* player = m_playerSystem->GetLocalPlayer())
        {
            m_lastPlayerX = player->posX;
            m_lastPlayerZ = player->posZ;
            m_visitedAreas.insert(player->currentAreaId);
            m_achievementSystem->SetStat(m_achievements, "areas_visited", static_cast<int>(m_visitedAreas.size()));
        }
    }

    std::string MMOGameplaySession::ResetDemo()
    {
        if (!m_playerSystem)
            return "MMO gameplay session is not initialized";

        m_accountId = 0;
        m_characterId = 1;
        if (!m_playerSystem->ConfigureLocalPlayer("Player", 1, 100.0f, 1, 0.0f, 1.0f, 0.0f))
            return "Local player is unavailable";

        InitializePlayerProgress();
        m_active = true;
        m_lastEvent = "Demo character ready";
        return GetStatusString();
    }

    std::string MMOGameplaySession::ActivateCharacter(uint32_t accountId, uint32_t characterId, const std::string& name,
                                                      int level, float maxHealth, uint32_t areaId, float x, float y,
                                                      float z)
    {
        if (!m_playerSystem || accountId == 0 || characterId == 0 || name.empty() || level < 1 ||
            !std::isfinite(maxHealth) || maxHealth <= 0.0f || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(z))
            return "Invalid character selection";
        if (!m_playerSystem->ConfigureLocalPlayer(name, level, maxHealth, areaId, x, y, z))
            return "Unable to activate character";

        m_accountId = accountId;
        m_characterId = characterId;
        InitializePlayerProgress();
        m_active = true;
        m_lastEvent = "Entered the world as " + name;
        return m_lastEvent;
    }

    float MMOGameplaySession::SpawnCoordinate(float minimum, float maximum)
    {
        return minimum + (maximum - minimum) * 0.5f;
    }

    std::string MMOGameplaySession::Travel(uint32_t areaId)
    {
        const auto* area = m_worldSetup ? m_worldSetup->GetArea(areaId) : nullptr;
        if (!area)
            return "Unknown area ID " + std::to_string(areaId);

        const float x = SpawnCoordinate(area->boundsMinX, area->boundsMaxX);
        const float y = std::clamp(1.0f, area->boundsMinY, area->boundsMaxY);
        const float z = SpawnCoordinate(area->boundsMinZ, area->boundsMaxZ);
        if (!m_playerSystem->TeleportLocalPlayer(areaId, x, y, z))
            return "Travel failed";

        m_visitedAreas.insert(areaId);
        m_achievementSystem->SetStat(m_achievements, "areas_visited",
                                     std::max(m_areaVisitCountFloor, static_cast<int>(m_visitedAreas.size())));
        m_lastEvent = "Traveled to " + area->name;
        return m_lastEvent;
    }

    std::string MMOGameplaySession::Gather(uint32_t itemId, int count)
    {
        const auto* item = m_inventorySystem ? m_inventorySystem->GetItem(itemId) : nullptr;
        if (!item || item->category != ItemCategory::Material || count <= 0)
            return "Usage: mmo_gather <material-item-id> [count]";

        const int added = m_inventorySystem->AddItem(m_inventory, itemId, count);
        if (added <= 0)
            return "Inventory cannot hold more " + item->name;

        const uint32_t areaId = m_playerSystem->GetLocalPlayer() ? m_playerSystem->GetLocalPlayer()->currentAreaId : 1;
        if (m_reputationSystem->GetFaction(areaId))
            m_reputationSystem->AddReputation(m_reputation, areaId, added);
        m_lastEvent = "Gathered " + item->name + " x" + std::to_string(added);
        return m_lastEvent;
    }

    std::string MMOGameplaySession::Craft(uint32_t recipeId)
    {
        const auto* recipe = m_craftingSystem ? m_craftingSystem->GetRecipe(recipeId) : nullptr;
        if (!recipe)
            return "Unknown recipe ID " + std::to_string(recipeId);
        if (!m_craftingSystem->StartCraft(m_crafting, m_inventory, recipeId))
            return "Cannot craft " + recipe->name + " (check recipe, skill, station, materials, and bag space)";

        m_lastEvent = "Crafting " + recipe->name;
        return m_lastEvent;
    }

    std::string MMOGameplaySession::UseItem(uint32_t itemId)
    {
        if (!m_inventorySystem || !m_inventorySystem->HasItem(m_inventory, itemId))
            return "Item is not in the active inventory";

        float healAmount = 0.0f;
        if (itemId == 1)
            healAmount = 50.0f;
        else if (itemId == 5 || itemId == 51)
            healAmount = 20.0f;
        else
            return "That item has no demo-use action";

        if (!m_playerSystem->HealLocalPlayer(healAmount))
            return "Healing item cannot be used at full health or while defeated";
        m_inventorySystem->RemoveItem(m_inventory, itemId, 1);
        m_lastEvent = "Used " + m_inventorySystem->GetItem(itemId)->name;
        return m_lastEvent;
    }

    void MMOGameplaySession::AwardLoot(const std::string& lootTable, int rolls)
    {
        for (const auto& stack : m_inventorySystem->RollLootTable(lootTable, rolls))
            m_inventorySystem->AddItem(m_inventory, stack.itemDefId, stack.count);
    }

    std::string MMOGameplaySession::AttackWorldBoss(uint32_t bossId)
    {
        const auto* definition = m_worldBossSystem ? m_worldBossSystem->GetBossDef(bossId) : nullptr;
        if (!definition)
            return "Unknown world boss ID " + std::to_string(bossId);
        if (!m_worldBossSystem->IsBossActive(bossId) && !m_worldBossSystem->SpawnBoss(bossId))
            return "World boss is unavailable";

        const auto* player = m_playerSystem->GetLocalPlayer();
        if (!player || player->health <= 0.0f)
            return "The local player is defeated";
        if (!m_worldBossSystem->DamageBoss(bossId, m_characterId, player->name, DEMO_BOSS_DAMAGE))
            return "World boss attack failed";

        const auto* instance = m_worldBossSystem->GetBossInstance(bossId);
        if (instance && !instance->IsAlive())
        {
            AwardLoot(definition->lootTable, 2);
            m_achievementSystem->IncrementStat(m_achievements, "kills_total");
            m_achievementSystem->IncrementStat(m_achievements, "bosses_killed");
            m_reputationSystem->AddReputation(m_reputation, 2, 50);
            m_lastEvent = "Defeated " + definition->name + " and received boss loot";
        }
        else
        {
            const int healthPercent = instance ? static_cast<int>(instance->GetHealthPct() * 100.0f) : 0;
            m_lastEvent = "Hit " + definition->name + " - " + std::to_string(healthPercent) + "% health remains";
        }
        return m_lastEvent;
    }

    std::string MMOGameplaySession::EnterDungeon(uint32_t dungeonId)
    {
        if (m_dungeonState.InDungeon())
            return "Already inside dungeon instance " + std::to_string(m_dungeonState.currentInstanceId);
        const auto* dungeon = m_dungeonSystem ? m_dungeonSystem->GetDungeon(dungeonId) : nullptr;
        if (!dungeon)
            return "Unknown dungeon ID " + std::to_string(dungeonId);

        const uint32_t instanceId =
            m_dungeonSystem->CreateInstance(dungeonId, DungeonDifficulty::Normal, {m_characterId});
        if (instanceId == 0)
            return "Unable to create dungeon instance";
        m_dungeonState.currentInstanceId = instanceId;
        m_lastEvent = "Entered " + dungeon->name + " (instance " + std::to_string(instanceId) + ")";
        return m_lastEvent;
    }

    std::string MMOGameplaySession::DefeatNextDungeonBoss()
    {
        if (!m_dungeonState.InDungeon())
            return "Enter a dungeon first";
        const auto* instance = m_dungeonSystem->GetInstance(m_dungeonState.currentInstanceId);
        if (!instance)
        {
            m_dungeonState.currentInstanceId = 0;
            return "Dungeon instance expired";
        }

        uint32_t bossId = 0;
        for (const auto& boss : instance->bosses)
        {
            if (!boss.defeated)
            {
                bossId = boss.id;
                break;
            }
        }
        if (bossId == 0 || !m_dungeonSystem->DefeatBoss(instance->instanceId, bossId))
            return "No active dungeon boss remains";

        ++m_dungeonState.bossesKilled;
        m_achievementSystem->IncrementStat(m_achievements, "bosses_killed");
        const auto* updated = m_dungeonSystem->GetInstance(instance->instanceId);
        if (updated && updated->state == InstanceState::Completed)
        {
            ++m_dungeonState.dungeonsCompleted;
            m_achievementSystem->IncrementStat(m_achievements, "dungeons_completed");
            if (const auto* def = m_dungeonSystem->GetDungeon(updated->dungeonDefId))
            {
                m_dungeonSystem->AddBossLockout(m_dungeonState, def->id, updated->difficulty, bossId,
                                                def->lockoutDuration);
            }
            m_dungeonState.currentInstanceId = 0;
            AwardLoot("mob_elite", 2);
            m_lastEvent = "Dungeon complete - elite loot awarded";
        }
        else
        {
            m_lastEvent = "Dungeon boss defeated";
        }
        return m_lastEvent;
    }

    std::string MMOGameplaySession::TakeDamage(float amount)
    {
        if (!m_playerSystem->DamageLocalPlayer(amount))
            return "Damage must be positive and the player must be alive";
        if (const auto* player = m_playerSystem->GetLocalPlayer(); player && player->health <= 0.0f)
        {
            m_respawnTimer = RESPAWN_DELAY;
            m_lastEvent = "Defeated - automatic respawn in 3 seconds";
        }
        else
        {
            m_lastEvent = "Took " + std::to_string(static_cast<int>(amount)) + " damage";
        }
        return m_lastEvent;
    }

    std::string MMOGameplaySession::Respawn()
    {
        const auto* town = m_worldSetup ? m_worldSetup->GetArea(1) : nullptr;
        if (!town)
            return "Respawn area unavailable";
        if (!m_playerSystem->RespawnLocalPlayer(1, 0.0f, 1.0f, 0.0f))
            return "Respawn is only available while defeated";
        m_respawnTimer = 0.0f;
        m_lastEvent = "Respawned in " + town->name;
        return m_lastEvent;
    }

    void MMOGameplaySession::Update(float deltaTime)
    {
        if (!m_active)
            return;

        const float safeDelta = std::max(deltaTime, 0.0f);
        m_playTime += safeDelta;
        const bool wasCrafting = m_crafting.isCrafting;
        m_craftingSystem->Update(safeDelta, m_crafting, m_inventory);
        if (wasCrafting && !m_crafting.isCrafting)
        {
            m_achievementSystem->IncrementStat(m_achievements, "items_crafted");
            m_lastEvent = "Craft complete";
        }
        m_dungeonSystem->UpdateLockouts(m_dungeonState, safeDelta);

        if (const auto* player = m_playerSystem->GetLocalPlayer())
        {
            const float deltaX = player->posX - m_lastPlayerX;
            const float deltaZ = player->posZ - m_lastPlayerZ;
            m_distanceTraveled += std::sqrt(deltaX * deltaX + deltaZ * deltaZ);
            m_lastPlayerX = player->posX;
            m_lastPlayerZ = player->posZ;
            m_visitedAreas.insert(player->currentAreaId);
            m_achievementSystem->SetStat(m_achievements, "distance_traveled", static_cast<int>(m_distanceTraveled));
            m_achievementSystem->SetStat(m_achievements, "areas_visited",
                                         std::max(m_areaVisitCountFloor, static_cast<int>(m_visitedAreas.size())));
        }

        if (m_respawnTimer > 0.0f)
        {
            m_respawnTimer -= safeDelta;
            if (m_respawnTimer <= 0.0f)
                Respawn();
        }
    }

    CharacterSaveData MMOGameplaySession::BuildSaveData() const
    {
        CharacterSaveData data;
        data.accountId = m_accountId;
        data.characterId = m_characterId;
        data.playTime = m_playTime;
        data.inventory = m_inventory;
        data.craftingState = m_crafting;
        data.achievementState = m_achievements;
        data.reputationState = m_reputation;
        data.dungeonState = m_dungeonState;

        if (const auto* player = m_playerSystem ? m_playerSystem->GetLocalPlayer() : nullptr)
        {
            data.name = player->name;
            data.level = player->level;
            data.areaId = player->currentAreaId;
            data.posX = player->posX;
            data.posY = player->posY;
            data.posZ = player->posZ;
            data.health = player->health;
            data.maxHealth = player->maxHealth;
        }
        return data;
    }

    bool MMOGameplaySession::LoadSaveData(const CharacterSaveData& data)
    {
        if (!m_playerSystem || data.characterId == 0 || data.name.empty() || !std::isfinite(data.maxHealth) ||
            data.maxHealth <= 0.0f || !std::isfinite(data.health) || !std::isfinite(data.posX) ||
            !std::isfinite(data.posY) || !std::isfinite(data.posZ))
            return false;
        if (!m_playerSystem->ConfigureLocalPlayer(data.name, std::max(data.level, 1), data.maxHealth, data.areaId,
                                                  data.posX, data.posY, data.posZ))
        {
            return false;
        }

        if (data.health < data.maxHealth)
            m_playerSystem->DamageLocalPlayer(data.maxHealth - std::max(data.health, 0.0f));
        m_accountId = data.accountId;
        m_characterId = data.characterId;
        m_playTime = std::max(data.playTime, 0.0f);
        m_inventory = data.inventory;
        m_crafting = data.craftingState;
        m_achievements = data.achievementState;
        m_reputation = data.reputationState;
        m_dungeonState = data.dungeonState;
        m_lastPlayerX = data.posX;
        m_lastPlayerZ = data.posZ;
        if (const auto it = m_achievements.stats.find("distance_traveled"); it != m_achievements.stats.end())
            m_distanceTraveled = static_cast<float>(std::max(it->second, 0));
        else
            m_distanceTraveled = 0.0f;
        if (const auto it = m_achievements.stats.find("areas_visited"); it != m_achievements.stats.end())
            m_areaVisitCountFloor = std::max(it->second, 0);
        else
            m_areaVisitCountFloor = 0;
        m_visitedAreas.clear();
        m_visitedAreas.insert(data.areaId);
        m_respawnTimer = data.health <= 0.0f ? RESPAWN_DELAY : 0.0f;
        m_active = true;
        m_lastEvent = "Loaded character " + data.name;
        return true;
    }

    std::string MMOGameplaySession::GetStatusString() const
    {
        std::ostringstream status;
        status << "=== MMO Playable Session ===\n";
        status << (m_playerSystem ? m_playerSystem->GetLocalPlayerStatusString() : "Local player unavailable\n");
        status << "Inventory: " << m_inventory.slots.size() << "/" << m_inventory.maxSlots << " slots, "
               << m_inventory.currency << " gold\n";
        status << "Crafting: " << (m_crafting.isCrafting ? "active" : "idle") << "\n";
        status << "Dungeons: " << m_dungeonState.dungeonsCompleted << " completed, " << m_dungeonState.bossesKilled
               << " bosses defeated\n";
        status << "Achievement points: " << m_achievements.totalPoints << "\n";
        if (!m_lastEvent.empty())
            status << "Last event: " << m_lastEvent << "\n";
        return status.str();
    }

    void MMOGameplaySession::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("MMO Playable Session", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::TextUnformatted("WASD: move | Shift: sprint");
        ImGui::TextWrapped("%s", GetStatusString().c_str());
        if (ImGui::Button("Gather Herbs"))
            Gather(10, 2);
        ImGui::SameLine();
        if (ImGui::Button("Craft Potion"))
            Craft(100);
        ImGui::SameLine();
        if (ImGui::Button("Use Potion"))
            UseItem(1);
        if (ImGui::Button("Attack Golem"))
            AttackWorldBoss(1);
        ImGui::SameLine();
        if (ImGui::Button("Enter Dungeon"))
            EnterDungeon(1);
        ImGui::SameLine();
        if (ImGui::Button("Defeat Dungeon Boss"))
            DefeatNextDungeonBoss();
#endif
    }

} // namespace MMO
