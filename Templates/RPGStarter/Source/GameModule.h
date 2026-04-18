/**
 * @file GameModule.h
 * @brief RPGStarter — RPG game module
 *
 * Role-playing game template with inventory, dialogue, quest tracking,
 * ability system, and save/load support. Extend this for your RPG.
 */

#pragma once

#include <Spark/SparkSDK.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Character stats
// ============================================================================

struct CharacterStats
{
    std::string name = "Hero";
    uint32_t level = 1;
    uint32_t experience = 0;
    uint32_t experienceToNext = 100;
    float health = 100.0f;
    float maxHealth = 100.0f;
    float mana = 50.0f;
    float maxMana = 50.0f;
    float strength = 10.0f;
    float dexterity = 10.0f;
    float intelligence = 10.0f;
    uint32_t gold = 0;
};

// ============================================================================
// Quest
// ============================================================================

struct QuestEntry
{
    uint32_t id = 0;
    std::string name;
    std::string description;
    bool isActive = false;
    bool isCompleted = false;
    uint32_t currentObjective = 0;
    uint32_t totalObjectives = 1;
};

// ============================================================================
// RPG Game Module
// ============================================================================

class RPGStarterModule : public Spark::IModule
{
  public:
    RPGStarterModule() = default;
    ~RPGStarterModule() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "RPGStarter";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        m_player = CharacterStats{};
        InitializeQuests();
        return true;
    }

    void OnUnload() override { m_context = nullptr; }

    void OnUpdate(float deltaTime) override
    {
        // Update dialogue, quest tracking, NPC AI, etc.
        (void)deltaTime;
    }

    void OnRender() override
    {
        // Render character portrait, quest tracker, dialogue box, minimap
    }

  private:
    void InitializeQuests()
    {
        m_quests.push_back({1, "The Beginning", "Speak with the village elder.", false, false, 0, 1});
        m_quests.push_back({2, "Lost Artifact", "Find the ancient relic in the cave.", false, false, 0, 3});
        m_quests.push_back({3, "Dragon's End", "Defeat the dragon threatening the kingdom.", false, false, 0, 5});
    }

    void GainExperience(uint32_t amount)
    {
        m_player.experience += amount;
        while (m_player.experience >= m_player.experienceToNext)
        {
            m_player.experience -= m_player.experienceToNext;
            m_player.level++;
            m_player.experienceToNext = m_player.level * 100;
            m_player.maxHealth += 10.0f;
            m_player.health = m_player.maxHealth;
            m_player.maxMana += 5.0f;
            m_player.mana = m_player.maxMana;
        }
    }

    Spark::IEngineContext* m_context = nullptr;
    CharacterStats m_player;
    std::vector<QuestEntry> m_quests;
};
