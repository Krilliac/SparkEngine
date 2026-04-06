/**
 * @file TestGameModuleRPG.cpp
 * @brief Tests for SparkGameRPG module: character, combat, and quest systems
 *
 * All tests are gated behind SPARK_TEST_HAS_IMGUI since the RPG module
 * .cpp files depend on ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRPG/Source/Character/RPGCharacterSystem.h"
#include "../GameModules/SparkGameRPG/Source/Combat/RPGCombatSystem.h"
#include "../GameModules/SparkGameRPG/Source/Quest/RPGQuestSystem.h"

using namespace RPG;

// =============================================================================
// RPGCharacterSystem
// =============================================================================

TEST(RPG_Character_Initialize)
{
    RPGCharacterSystem charSys;
    bool ok = charSys.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(RPG_Character_GetClassCount)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    EXPECT_TRUE(charSys.GetClassCount() > 0);
}

TEST(RPG_Character_CreateCharacter)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    uint32_t id = charSys.CreateCharacter("TestHero", CharacterClass::Warrior);
    EXPECT_TRUE(id > 0);
}

TEST(RPG_Character_GetCharacter)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    uint32_t id = charSys.CreateCharacter("Aldric", CharacterClass::Mage);
    const CharacterData* data = charSys.GetCharacter(id);
    EXPECT_TRUE(data != nullptr);
    EXPECT_EQ(data->name, std::string("Aldric"));
    EXPECT_EQ(static_cast<uint8_t>(data->classId), static_cast<uint8_t>(CharacterClass::Mage));
}

TEST(RPG_Character_AddXP_LevelUp)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    uint32_t id = charSys.CreateCharacter("Leveler", CharacterClass::Warrior);
    const CharacterData* data = charSys.GetCharacter(id);
    int startLevel = data->level;

    // Add enough XP to trigger at least one level up
    charSys.AddXP(id, 5000);
    EXPECT_TRUE(data->level > startLevel);
}

TEST(RPG_Character_ComputeEffectiveStats)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    uint32_t id = charSys.CreateCharacter("StatCheck", CharacterClass::Ranger);
    CharacterStats stats = charSys.ComputeEffectiveStats(id);
    EXPECT_TRUE(stats.strength > 0.0f);
    EXPECT_TRUE(stats.dexterity > 0.0f);
    EXPECT_TRUE(stats.intelligence > 0.0f);
}

TEST(RPG_Character_AllocateStatPoint)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    uint32_t id = charSys.CreateCharacter("Allocator", CharacterClass::Cleric);

    // Level up to get stat points
    charSys.AddXP(id, 5000);
    CharacterStats before = charSys.ComputeEffectiveStats(id);
    charSys.AllocateStatPoint(id, "strength");
    CharacterStats after = charSys.ComputeEffectiveStats(id);
    EXPECT_TRUE(after.strength >= before.strength);
}

TEST(RPG_Character_EquipUnequip)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    uint32_t id = charSys.CreateCharacter("Gearhead", CharacterClass::Paladin);

    CharacterStats bonus;
    bonus.strength = 5.0f;
    charSys.Equip(id, ItemSlot::MainHand, 100, bonus);

    CharacterStats equipped = charSys.ComputeEffectiveStats(id);

    charSys.Unequip(id, ItemSlot::MainHand);
    CharacterStats unequipped = charSys.ComputeEffectiveStats(id);

    EXPECT_TRUE(equipped.strength > unequipped.strength);
}

TEST(RPG_Character_GetClassListString)
{
    RPGCharacterSystem charSys;
    charSys.Initialize(nullptr);
    std::string list = charSys.GetClassListString();
    EXPECT_TRUE(!list.empty());
}

// =============================================================================
// RPGCombatSystem
// =============================================================================

TEST(RPG_Combat_Initialize)
{
    RPGCombatSystem combat;
    bool ok = combat.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(RPG_Combat_StartEncounter)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);
    uint32_t encId = combat.StartEncounter(1, 2);
    EXPECT_TRUE(encId > 0);
}

TEST(RPG_Combat_GetActiveCombatCount)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);
    EXPECT_EQ(combat.GetActiveCombatCount(), 0u);
    combat.StartEncounter(1, 2);
    EXPECT_EQ(combat.GetActiveCombatCount(), 1u);
    combat.StartEncounter(3, 4);
    EXPECT_EQ(combat.GetActiveCombatCount(), 2u);
}

TEST(RPG_Combat_CalculateDamage)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);
    ResistanceProfile noResist;
    ComboState noCombo;
    DamageResult result = combat.CalculateDamage(50.0f, DamageType::Physical, 20.0f, 10.0f, noResist, noCombo);
    EXPECT_TRUE(result.rawDamage > 0.0f);
    EXPECT_TRUE(result.mitigatedDamage > 0.0f);
}

TEST(RPG_Combat_CalculateKnockback)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);
    float kb = combat.CalculateKnockback(100.0f, DamageType::Physical);
    EXPECT_TRUE(kb > 0.0f);
}

TEST(RPG_Combat_UseAbility_Cooldown)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);
    bool used = combat.UseAbility(1, 100, 5.0f);
    EXPECT_TRUE(used);

    // Ability should be on cooldown now
    EXPECT_FALSE(combat.IsAbilityReady(1, 100));
}

TEST(RPG_Combat_ComboSystem)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);

    combat.RegisterHit(1);
    combat.RegisterHit(1);
    combat.RegisterHit(1);
    const ComboState* combo = combat.GetComboState(1);
    EXPECT_TRUE(combo != nullptr);
    EXPECT_TRUE(combo->hitCount > 0);
    EXPECT_TRUE(combo->damageMultiplier > 1.0f);

    combat.ResetCombo(1);
    combo = combat.GetComboState(1);
    EXPECT_TRUE(combo == nullptr || combo->hitCount == 0);
}

TEST(RPG_Combat_EndEncounter)
{
    RPGCombatSystem combat;
    combat.Initialize(nullptr);
    uint32_t encId = combat.StartEncounter(10, 20);
    EXPECT_EQ(combat.GetActiveCombatCount(), 1u);
    combat.EndEncounter(encId);
    EXPECT_EQ(combat.GetActiveCombatCount(), 0u);
}

// =============================================================================
// RPGQuestSystem
// =============================================================================

TEST(RPG_Quest_Initialize)
{
    RPGQuestSystem quests;
    bool ok = quests.Initialize(nullptr);
    EXPECT_TRUE(ok);
}

TEST(RPG_Quest_RegisterAndGetDef)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9001;
    def.name = "Test Quest";
    def.description = "A test quest";
    def.requiredLevel = 1;
    QuestObjective obj;
    obj.objectiveId = 1;
    obj.type = ObjectiveType::Kill;
    obj.description = "Kill 3 goblins";
    obj.targetId = 10;
    obj.requiredCount = 3;
    def.objectives.push_back(obj);
    quests.RegisterQuest(def);

    const QuestDef* fetched = quests.GetQuestDef(9001);
    EXPECT_TRUE(fetched != nullptr);
    EXPECT_EQ(fetched->name, std::string("Test Quest"));
}

TEST(RPG_Quest_GetQuestCount)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);
    size_t initial = quests.GetQuestCount();

    QuestDef def;
    def.questId = 9002;
    def.name = "Counter Quest";
    def.requiredLevel = 1;
    quests.RegisterQuest(def);

    EXPECT_EQ(quests.GetQuestCount(), initial + 1);
}

TEST(RPG_Quest_AcceptQuest)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9003;
    def.name = "Accept Test";
    def.requiredLevel = 1;
    QuestObjective obj;
    obj.objectiveId = 1;
    obj.type = ObjectiveType::Collect;
    obj.requiredCount = 5;
    def.objectives.push_back(obj);
    quests.RegisterQuest(def);

    bool accepted = quests.AcceptQuest(1, 9003);
    EXPECT_TRUE(accepted);

    auto active = quests.GetActiveQuests(1);
    EXPECT_TRUE(!active.empty());
}

TEST(RPG_Quest_UpdateObjective)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9004;
    def.name = "Objective Test";
    def.requiredLevel = 1;
    QuestObjective obj;
    obj.objectiveId = 1;
    obj.type = ObjectiveType::Kill;
    obj.targetId = 42;
    obj.requiredCount = 3;
    def.objectives.push_back(obj);
    quests.RegisterQuest(def);
    quests.AcceptQuest(1, 9004);

    quests.UpdateObjective(1, ObjectiveType::Kill, 42, 1);
    const QuestProgress* prog = quests.GetQuestProgress(1, 9004);
    EXPECT_TRUE(prog != nullptr);
    EXPECT_TRUE(prog->objectives[0].currentCount > 0);
}

TEST(RPG_Quest_CompleteQuest)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9005;
    def.name = "Complete Test";
    def.requiredLevel = 1;
    QuestObjective obj;
    obj.objectiveId = 1;
    obj.type = ObjectiveType::Kill;
    obj.targetId = 50;
    obj.requiredCount = 1;
    def.objectives.push_back(obj);
    quests.RegisterQuest(def);
    quests.AcceptQuest(1, 9005);

    quests.UpdateObjective(1, ObjectiveType::Kill, 50, 1);
    bool completed = quests.CompleteQuest(1, 9005);
    EXPECT_TRUE(completed);

    auto done = quests.GetCompletedQuests(1);
    EXPECT_TRUE(!done.empty());
}

TEST(RPG_Quest_AbandonQuest)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9006;
    def.name = "Abandon Test";
    def.requiredLevel = 1;
    quests.RegisterQuest(def);
    quests.AcceptQuest(1, 9006);

    bool abandoned = quests.AbandonQuest(1, 9006);
    EXPECT_TRUE(abandoned);

    auto active = quests.GetActiveQuests(1);
    // The abandoned quest should no longer be active
    bool found = false;
    for (const auto* q : active)
    {
        if (q->questId == 9006)
            found = true;
    }
    EXPECT_FALSE(found);
}

TEST(RPG_Quest_IsQuestAvailable_LevelGate)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9007;
    def.name = "Level Gate Test";
    def.requiredLevel = 10;
    quests.RegisterQuest(def);

    // Character level 1 should not meet the level requirement
    EXPECT_FALSE(quests.IsQuestAvailable(1, 9007, 1));
    // Character level 10 should meet it
    EXPECT_TRUE(quests.IsQuestAvailable(1, 9007, 10));
}

TEST(RPG_Quest_GetQuestListString)
{
    RPGQuestSystem quests;
    quests.Initialize(nullptr);

    QuestDef def;
    def.questId = 9008;
    def.name = "String Test";
    def.requiredLevel = 1;
    quests.RegisterQuest(def);

    std::string list = quests.GetQuestListString();
    EXPECT_TRUE(!list.empty());
}

#endif // SPARK_TEST_HAS_IMGUI
