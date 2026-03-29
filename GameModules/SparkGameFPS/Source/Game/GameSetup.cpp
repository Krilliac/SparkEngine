/**
 * @file GameSetup.cpp
 * @brief Game initialization helpers — scene creation, system setup
 *
 * Extracted from Game.cpp to keep init/setup logic separate from
 * the per-frame game loop and rendering.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdint>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

#include "Game.h"
#include "ClassSystem.h"
#include "Enemy.h"
#include "WaveSpawner.h"
#include "ProgressionSystem.h"
#include "LootSystem.h"
#include "ArenaBuilder.h"
#include "Utils/SparkConsole.h"

#include "Graphics/GraphicsEngine.h"
#include "CubeObject.h"
#include "PlaneObject.h"
#include "SphereObject.h"
#include "WallObject.h"
#include "ModelObject.h"
#include "Player.h"
#include "Engine/Events/EventSystem.h"

#include "Utils/LogMacros.h"

using namespace DirectX;

/*-------------------------------------------------------------
  Extracted init helpers — called from Game::Initialize()
--------------------------------------------------------------*/

void Game::InitializeInteractionObjects()
{
    m_interactionSystem = std::make_unique<Spark::InteractionSystem>();
    m_interactionSystem->Initialize();
    m_player->SetInteractionSystem(m_interactionSystem.get());

    // NOTE: Interactive objects and damage zones are now defined in the scene file
    // (Assets/Scenes/level1.scene) as [Interaction] and [TriggerVolume] entries.
    // They can be placed and edited in the SparkEditor without recompiling.
    // The code below shows the equivalent C++ approach for reference.

    // --- Code-based approach (kept for reference; scene file is now primary) ---
    // auto* dev = m_graphics->GetDevice();
    // auto* ctx = m_graphics->GetContext();
    //
    // // Doors
    // auto* door1 = m_interactionSystem->SpawnDoor({10.0f, 1.5f, 0.0f}, {0.2f, 3.0f, 2.0f}, dev, ctx);
    // if (door1) door1->SetSlideDirection({0, 1, 0});
    // m_interactionSystem->SpawnDoor({-10.0f, 1.5f, 5.0f}, {0.2f, 3.0f, 2.0f}, dev, ctx);
    //
    // // Class change terminals
    // m_interactionSystem->SpawnClassTerminal({0.0f, 0.5f, -15.0f}, m_classSystem.get(), dev, ctx);
    // m_interactionSystem->SpawnClassTerminal({15.0f, 0.5f, 15.0f}, m_classSystem.get(), dev, ctx);
    //
    // // Pickups
    // m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::HEALTH_PICKUP,
    //                                  {5.0f, 0.5f, 10.0f}, 50.0f, dev, ctx);
    // m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::ARMOR_PICKUP,
    //                                  {-5.0f, 0.5f, 10.0f}, 50.0f, dev, ctx);
    // m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::AMMO_PICKUP,
    //                                  {0.0f, 0.5f, 20.0f}, 60.0f, dev, ctx);
    // m_interactionSystem->SpawnPickup(SparkEditor::InteractiveObjectType::HEALTH_PICKUP,
    //                                  {-15.0f, 0.5f, -10.0f}, 25.0f, dev, ctx);
    //
    // // Jump pads
    // m_interactionSystem->SpawnJumpPad({12.0f, 0.1f, 12.0f}, 18.0f, dev, ctx);
    // m_interactionSystem->SpawnJumpPad({-12.0f, 0.1f, -12.0f}, 15.0f, dev, ctx);
    //
    // // Teleporter pair
    // m_interactionSystem->SpawnTeleporterPair({-20.0f, 0.5f, -20.0f}, {20.0f, 0.5f, 20.0f}, dev, ctx);
    //
    // // Elevator
    // m_interactionSystem->SpawnElevator({-8.0f, 0.0f, 0.0f}, {-8.0f, 12.0f, 0.0f}, dev, ctx);
    //
    // // Destructible barrels
    // m_interactionSystem->SpawnDestructible({7.0f, 0.5f, -5.0f}, 50.0f, dev, ctx);
    // m_interactionSystem->SpawnDestructible({8.0f, 0.5f, -5.0f}, 50.0f, dev, ctx);
    // m_interactionSystem->SpawnDestructible({7.5f, 1.3f, -5.0f}, 30.0f, dev, ctx);

    LOG_TO_CONSOLE_IMMEDIATE(L"Interaction objects loaded from scene file", L"SUCCESS");

    // Damage zone system still needs runtime initialization for callbacks
    m_damageZoneSystem = std::make_unique<Spark::DamageZoneSystem>();
    m_damageZoneSystem->Initialize();

    // NOTE: Damage zones are now defined in the scene file as [TriggerVolume] entries.
    // The code below shows the equivalent C++ approach for reference.
    // m_damageZoneSystem->CreateLavaZone("Arena_Lava_Pit", {0.0f, -2.0f, 0.0f}, {3.0f, 2.0f, 3.0f});
    // m_damageZoneSystem->CreateVoidZone("Arena_Boundary", {0.0f, -20.0f, 0.0f}, {100.0f, 5.0f, 100.0f});
    // m_damageZoneSystem->CreateElectricZone("Electric_Trap", {15.0f, 0.5f, -15.0f}, {3.0f, 2.0f, 3.0f});

    LOG_TO_CONSOLE_IMMEDIATE(L"Damage zones loaded from scene file", L"SUCCESS");
}

void Game::InitializeRespawnAndVehicles()
{
    m_respawnSystem = std::make_unique<Spark::RespawnSystem>();
    m_respawnSystem->Initialize();

    // NOTE: Spawn points are now defined in the scene file (Assets/Scenes/level1.scene)
    // as [SpawnPoint] entries with position, tag, and priority. They can be placed and
    // edited in the SparkEditor without recompiling.
    // The code below shows the equivalent C++ approach for reference.
    //
    // Spark::RespawnPoint spawn1;
    // spawn1.name = "North Spawn";
    // spawn1.position = {0.0f, 2.0f, -20.0f};
    // spawn1.priority = 1;
    // m_respawnSystem->AddSpawnPoint(spawn1);
    //
    // Spark::RespawnPoint spawn2;
    // spawn2.name = "South Spawn";
    // spawn2.position = {0.0f, 2.0f, 20.0f};
    // spawn2.priority = 1;
    // m_respawnSystem->AddSpawnPoint(spawn2);
    //
    // Spark::RespawnPoint spawn3;
    // spawn3.name = "East Spawn";
    // spawn3.position = {20.0f, 2.0f, 0.0f};
    // m_respawnSystem->AddSpawnPoint(spawn3);
    //
    // Spark::RespawnPoint spawn4;
    // spawn4.name = "West Spawn";
    // spawn4.position = {-20.0f, 2.0f, 0.0f};
    // m_respawnSystem->AddSpawnPoint(spawn4);

    LOG_TO_CONSOLE_IMMEDIATE(L"Spawn points loaded from scene file", L"SUCCESS");

    // NOTE: Vehicles are now defined in the scene file as [Vehicle] entries.
    // The code below shows the equivalent C++ approach for reference.
    //
    // auto* dev = m_graphics->GetDevice();
    // auto* ctx = m_graphics->GetContext();
    // m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::BUGGY, {25.0f, 0.5f, 0.0f}, dev, ctx);
    // m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::MOTORCYCLE, {-25.0f, 0.5f, 0.0f}, dev, ctx);
    // m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::TANK, {0.0f, 0.5f, 25.0f}, dev, ctx);
    // m_vehicleSystem->SpawnVehicle(SparkEditor::VehicleType::HELICOPTER, {0.0f, 5.0f, -25.0f}, dev, ctx);

    LOG_TO_CONSOLE_IMMEDIATE(L"Vehicles loaded from scene file", L"SUCCESS");
}

void Game::InitializeGameModeAndHUD()
{
    m_gameMode = std::make_unique<Spark::GameMode>();
    Spark::GameModeRules rules = Spark::GameMode::GetPreset(Spark::GameModeType::Deathmatch);
    m_gameMode->Initialize(rules);
    m_gameMode->AddPlayer("Player1");
    m_gameMode->StartMatch();
    LOG_TO_CONSOLE_IMMEDIATE(L"GameMode initialized (Deathmatch)", L"SUCCESS");

    m_hudSystem = std::make_unique<Spark::HUDSystem>();
    m_hudSystem->Initialize();
    m_hudSystem->SetPlayer(m_player.get());
    m_hudSystem->SetCurrentClass(GetPlayerClass());
    LOG_TO_CONSOLE_IMMEDIATE(L"HUD system initialized", L"SUCCESS");

    // Wire GameMode event callbacks to HUD system
    auto& events = m_gameMode->GetEvents();

    events.onPlayerKill = [this](const std::string&)
    {
        if (m_hudSystem)
            m_hudSystem->ShowHitMarker(false);
    };

    events.onPlayerDeath = [this](const std::string&)
    {
        if (m_hudSystem)
            m_hudSystem->AddDamageIndicator(0.0f, 1.0f, 2.0f);
    };

    events.onRoundStart = [this](int roundNum)
    {
        std::wstring msg = L"Round " + std::to_wstring(roundNum) + L" started";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
    };

    events.onRoundEnd = [this](const Spark::RoundResult& result)
    {
        std::wstring msg = L"Round " + std::to_wstring(result.roundNumber) + L" ended - MVP: " +
                           std::wstring(result.mvpPlayer.begin(), result.mvpPlayer.end());
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
    };

    events.onMatchEnd = [this](Spark::Team winner)
    {
        const char* teamName = (winner == Spark::Team::Alpha)   ? "Alpha"
                               : (winner == Spark::Team::Bravo) ? "Bravo"
                                                                : "None";
        std::string tn(teamName);
        std::wstring msg = L"Match ended - Winner: " + std::wstring(tn.begin(), tn.end());
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"INFO");
    };

    events.onKillStreak = [this](const std::string& player, int streak)
    {
        if (m_hudSystem)
        {
            std::string streakMsg = player + " is on a " + std::to_string(streak) + " kill streak!";
            m_hudSystem->AddKillFeedEntry("", player, streakMsg);
        }
    };

    events.onFirstBlood = [this](const std::string& player)
    {
        if (m_hudSystem)
            m_hudSystem->AddKillFeedEntry(player, "", "First Blood");
    };

    events.onScoreUpdate = [this](Spark::Team, int)
    {
        if (m_hudSystem && m_gameMode)
        {
            auto scores = m_gameMode->GetScoreboard();
            std::vector<Spark::ScoreboardEntry> entries;
            entries.reserve(scores.size());
            for (const auto& ps : scores)
            {
                Spark::ScoreboardEntry entry;
                entry.playerName = ps.playerName;
                entry.kills = ps.kills;
                entry.deaths = ps.deaths;
                entry.assists = ps.assists;
                entry.score = ps.totalScore;
                entry.isLocalPlayer = (ps.playerName == "Player1");
                entries.push_back(std::move(entry));
            }
            m_hudSystem->SetScoreboard(entries);
        }
    };

    LOG_TO_CONSOLE_IMMEDIATE(L"GameMode events wired to HUD", L"SUCCESS");
}

void Game::InitializeInventorySystem()
{
    Spark::ItemDef healthPotion;
    healthPotion.id = 1;
    healthPotion.name = "Health Potion";
    healthPotion.description = "Restores 50 HP";
    healthPotion.category = Spark::ItemCategory::Consumable;
    healthPotion.rarity = Spark::ItemRarity::Common;
    healthPotion.maxStackSize = 10;
    healthPotion.weight = 0.5f;
    m_itemRegistry.RegisterItem(healthPotion);

    Spark::ItemDef armorShard;
    armorShard.id = 2;
    armorShard.name = "Armor Shard";
    armorShard.description = "Adds 25 armor";
    armorShard.category = Spark::ItemCategory::Consumable;
    armorShard.rarity = Spark::ItemRarity::Common;
    armorShard.maxStackSize = 5;
    armorShard.weight = 0.3f;
    m_itemRegistry.RegisterItem(armorShard);

    Spark::ItemDef ammoPack;
    ammoPack.id = 3;
    ammoPack.name = "Ammo Pack";
    ammoPack.description = "Standard ammunition";
    ammoPack.category = Spark::ItemCategory::Material;
    ammoPack.rarity = Spark::ItemRarity::Common;
    ammoPack.maxStackSize = 20;
    ammoPack.weight = 0.2f;
    m_itemRegistry.RegisterItem(ammoPack);

    Spark::ItemDef rareWeapon;
    rareWeapon.id = 4;
    rareWeapon.name = "Plasma Rifle";
    rareWeapon.description = "High-energy plasma weapon";
    rareWeapon.category = Spark::ItemCategory::Weapon;
    rareWeapon.rarity = Spark::ItemRarity::Rare;
    rareWeapon.maxStackSize = 1;
    rareWeapon.weight = 5.0f;
    m_itemRegistry.RegisterItem(rareWeapon);

    m_playerInventory.maxSlots = 20;
    m_playerInventory.maxWeight = 50.0f;
    LOG_TO_CONSOLE_IMMEDIATE(L"Inventory system initialized (4 item types)", L"SUCCESS");
}

void Game::InitializeQuestSystem()
{
    Spark::QuestDef arenaQuest;
    arenaQuest.id = 1;
    arenaQuest.name = "Arena Champion";
    arenaQuest.description = "Prove your worth in the combat arena";
    arenaQuest.objectives.push_back({Spark::ObjectiveType::Kill, "Defeat 10 enemies", 0, 10});
    arenaQuest.objectives.push_back({Spark::ObjectiveType::Survive, "Survive for 120 seconds", 0, 1});
    arenaQuest.reward.experiencePoints = 500;
    m_questRegistry.RegisterQuest(arenaQuest);

    Spark::QuestDef exploreQuest;
    exploreQuest.id = 2;
    exploreQuest.name = "Explorer";
    exploreQuest.description = "Discover all gravity zones";
    exploreQuest.objectives.push_back({Spark::ObjectiveType::Reach, "Visit low-gravity zone", 0, 1});
    exploreQuest.objectives.push_back({Spark::ObjectiveType::Reach, "Visit zero-gravity zone", 0, 1});
    exploreQuest.objectives.push_back({Spark::ObjectiveType::Reach, "Visit reverse-gravity zone", 0, 1});
    exploreQuest.reward.experiencePoints = 300;
    m_questRegistry.RegisterQuest(exploreQuest);

    Spark::QuestDef collectQuest;
    collectQuest.id = 3;
    collectQuest.name = "Scavenger";
    collectQuest.description = "Collect supplies from the arena";
    collectQuest.objectives.push_back({Spark::ObjectiveType::Collect, "Collect 5 health potions", 1, 5});
    collectQuest.objectives.push_back({Spark::ObjectiveType::Collect, "Collect 3 ammo packs", 3, 3});
    collectQuest.reward.experiencePoints = 200;
    collectQuest.reward.currency = 100;
    m_questRegistry.RegisterQuest(collectQuest);

    // Auto-start the first quest
    Spark::QuestOps::StartQuest(m_playerQuests, m_questRegistry, 1);
    LOG_TO_CONSOLE_IMMEDIATE(L"Quest system initialized (3 quests, 1 active)", L"SUCCESS");
}

/*-------------------------------------------------------------
  Enemy spawning and management
--------------------------------------------------------------*/

Enemy* Game::SpawnEnemy(EnemyType type, float x, float y, float z)
{
    auto* dev = m_graphics->GetDevice();
    auto* ctx = m_graphics->GetContext();

    auto enemy = std::make_unique<Enemy>();
    HRESULT hr = enemy->Initialize(dev, ctx, type, m_player.get());
    if (FAILED(hr))
        return nullptr;

    enemy->SetPosition({x, y, z});
    enemy->SetName("Enemy_" + std::to_string(enemy->GetID()));

    Enemy* ptr = enemy.get();
    m_enemies.push_back(ptr);
    m_gameObjects.push_back(std::move(enemy));
    return ptr;
}

size_t Game::GetAliveEnemyCount() const
{
    size_t count = 0;
    for (const auto* e : m_enemies)
    {
        if (e && e->IsAlive())
            ++count;
    }
    return count;
}

void Game::InitializeEnemies()
{
    // Spawn AI enemies with patrol routes for the combat arena.
    // Grunts patrol cardinal positions around the arena perimeter.
    auto* g1 = SpawnEnemy(EnemyType::Grunt, 15.0f, 1.0f, 15.0f);
    auto* g2 = SpawnEnemy(EnemyType::Grunt, -15.0f, 1.0f, 15.0f);
    auto* g3 = SpawnEnemy(EnemyType::Grunt, 15.0f, 1.0f, -15.0f);
    auto* g4 = SpawnEnemy(EnemyType::Grunt, -15.0f, 1.0f, -15.0f);

    if (g1)
        g1->SetPatrolPoints({{15, 1, 15}, {15, 1, -5}, {5, 1, -5}});
    if (g2)
        g2->SetPatrolPoints({{-15, 1, 15}, {-15, 1, -5}, {-5, 1, -5}});
    if (g3)
        g3->SetPatrolPoints({{15, 1, -15}, {5, 1, -15}, {5, 1, -5}});
    if (g4)
        g4->SetPatrolPoints({{-15, 1, -15}, {-5, 1, -15}, {-5, 1, -5}});

    // Scouts — fast flankers on the sides
    SpawnEnemy(EnemyType::Scout, 10.0f, 1.0f, 0.0f);
    SpawnEnemy(EnemyType::Scout, -10.0f, 1.0f, 0.0f);

    // Guard — stationary sentry watching the center
    SpawnEnemy(EnemyType::Guard, 0.0f, 1.0f, -10.0f);

    // Heavy — arena boss near the back
    SpawnEnemy(EnemyType::Heavy, 0.0f, 1.0f, 18.0f);

    // Snipers — long-range on elevated platforms
    SpawnEnemy(EnemyType::Sniper, 22.0f, 4.0f, 0.0f);
    SpawnEnemy(EnemyType::Sniper, -22.0f, 4.0f, 0.0f);

    // Medic — stays near the guard, heals allies
    SpawnEnemy(EnemyType::Medic, 2.0f, 1.0f, -8.0f);

    std::wstring msg = L"AI enemies spawned: " + std::to_wstring(m_enemies.size()) +
                       L" (4 grunts, 2 scouts, 1 guard, 1 heavy, 2 snipers, 1 medic)";
    LOG_TO_CONSOLE_IMMEDIATE(msg, L"SUCCESS");
}

/*-------------------------------------------------------------
  Wave Spawner, Progression, and Loot Systems
--------------------------------------------------------------*/

void Game::InitializeGameplaySystems()
{
    // --- Wave Spawner ---
    m_waveSpawner = std::make_unique<Spark::WaveSpawner>();

    // NOTE: Wave spawn points are now defined in the scene file (Assets/Scenes/level1.scene)
    // as [SpawnPoint] entries with tag=wave_spawn. They can be placed and edited in the
    // SparkEditor without recompiling.
    // The code below shows the equivalent C++ approach for reference.
    //
    // std::vector<XMFLOAT3> enemySpawnPoints = {
    //     {20.0f, 1.0f, 20.0f}, {-20.0f, 1.0f, 20.0f},  {20.0f, 1.0f, -20.0f}, {-20.0f, 1.0f, -20.0f},
    //     {25.0f, 1.0f, 0.0f},  {-25.0f, 1.0f, 0.0f},   {0.0f, 1.0f, 25.0f},   {0.0f, 1.0f, -25.0f},
    //     {15.0f, 1.0f, 10.0f}, {-15.0f, 1.0f, -10.0f},
    // };

    // Initialize with empty points — scene loader will populate from [SpawnPoint] tag=wave_spawn
    std::vector<XMFLOAT3> enemySpawnPoints;
    m_waveSpawner->Initialize(enemySpawnPoints);

    // --- Progression ---
    m_progression = std::make_unique<Spark::ProgressionSystem>();
    m_progression->Initialize();

    // Wire progression callbacks to HUD
    m_progression->GetCallbacks().onLevelUp = [this](int newLevel, const Spark::LevelBonuses& bonuses)
    {
        if (m_hudSystem)
            m_hudSystem->AddKillFeedEntry("", "Player1", "LEVEL UP: " + std::to_string(newLevel));

        // Apply level bonuses to player
        if (m_player)
        {
            m_player->Console_SetMaxHealth(100.0f + bonuses.healthBonus);
            m_player->Console_SetHealth(m_player->GetMaxHealth()); // Full heal on level up
        }
    };

    m_progression->GetCallbacks().onUnlock = [this](const Spark::LevelUnlock& unlock)
    {
        if (m_hudSystem)
            m_hudSystem->AddKillFeedEntry("", "UNLOCK", unlock.name + " - " + unlock.description);
    };

    // --- Loot System ---
    m_lootSystem = std::make_unique<Spark::LootSystem>();
    m_lootSystem->Initialize();

    // Wire loot callbacks
    m_lootSystem->GetCallbacks().onPowerUpCollected = [this](Spark::PowerUpType type, float duration)
    {
        if (m_hudSystem)
        {
            std::string msg =
                std::string(Spark::LootSystem::GetPowerUpName(type)) + " (" + std::to_string((int)duration) + "s)";
            m_hudSystem->AddKillFeedEntry("", "POWER-UP", msg);
        }
    };

    // Wire wave callbacks
    m_waveSpawner->GetCallbacks().onWaveStart = [this](int waveNum, const std::string& announcement)
    {
        if (m_hudSystem)
            m_hudSystem->AddKillFeedEntry("", "WAVE", announcement);
        LOG_TO_CONSOLE_IMMEDIATE(std::wstring(announcement.begin(), announcement.end()) + L" starting!", L"INFO");
    };

    m_waveSpawner->GetCallbacks().onWaveComplete = [this](int waveNum, int killed)
    {
        // Award XP for wave clear
        if (m_progression)
            m_progression->AwardXP(Spark::ProgressionSystem::XP_PER_WAVE_CLEAR, "wave_clear");

        if (m_hudSystem)
        {
            std::string msg = "Wave " + std::to_string(waveNum) + " cleared! +" +
                              std::to_string(Spark::ProgressionSystem::XP_PER_WAVE_CLEAR) + " XP";
            m_hudSystem->AddKillFeedEntry("", "WAVE", msg);
        }
    };

    m_waveSpawner->GetCallbacks().onAllWavesComplete = [this](int totalWaves)
    {
        if (m_hudSystem)
            m_hudSystem->AddKillFeedEntry("", "VICTORY", "All " + std::to_string(totalWaves) + " waves cleared!");
    };

    // Wire enemy kill events to progression and loot
    if (m_eventBus)
    {
        (void)m_eventBus->Subscribe<Spark::EntityKilledEvent>(
            [this](const Spark::EntityKilledEvent& e)
            {
                // Award XP for kills
                if (m_progression)
                {
                    int xp = Spark::ProgressionSystem::XP_PER_KILL;
                    if (m_lootSystem && m_lootSystem->HasBuff(Spark::PowerUpType::DoubleXP))
                        xp *= 2;
                    m_progression->AwardXP(xp, "kill");
                }

                // Spawn loot near player as we don't have enemy position in the event
                if (m_lootSystem && m_player)
                {
                    XMFLOAT3 deathPos = m_player->GetPosition();
                    // Offset slightly so drops don't stack on player
                    deathPos.x += 2.0f;
                    deathPos.z += 2.0f;
                    bool isBoss = m_waveSpawner && m_waveSpawner->IsBossWave();
                    m_lootSystem->SpawnEnemyLoot(deathPos, 0, isBoss);
                }
            });
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Gameplay systems initialized (waves, progression, loot)", L"SUCCESS");
}

void Game::StartWaves()
{
    if (m_waveSpawner)
    {
        m_waveSpawner->Start();
        LOG_TO_CONSOLE_IMMEDIATE(L"Wave mode started!", L"SUCCESS");
    }
}
