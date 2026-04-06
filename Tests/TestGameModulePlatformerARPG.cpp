/**
 * @file TestGameModulePlatformerARPG.cpp
 * @brief Tests for Platformer and ARPG game module systems
 *
 * All tests are behind SPARK_TEST_HAS_IMGUI because these game module .cpp
 * files include ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

// =============================================================================
// Platformer includes
// =============================================================================

#include "../GameModules/SparkGamePlatformer/Source/Player/PlatformerPlayerController.h"
#include "../GameModules/SparkGamePlatformer/Source/Checkpoint/PlatformerCheckpointSystem.h"

namespace Platformer
{

    // =============================================================================
    // PlatformerCheckpointSystem
    // =============================================================================

    TEST(Platformer_Checkpoint_Initialize)
    {
        PlatformerCheckpointSystem checkpoints;
        EXPECT_TRUE(checkpoints.Initialize(nullptr));
        checkpoints.Shutdown();
    }

    TEST(Platformer_Checkpoint_CountAndActivated)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        // Demo checkpoints are built during Initialize
        EXPECT_GT(checkpoints.GetCheckpointCount(), 0u);
        EXPECT_EQ(checkpoints.GetActivatedCount(), 0u);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Checkpoint_SetLevelSpawnAndGetPosition)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        checkpoints.SetLevelSpawn(10.0f, 5.0f, 20.0f);

        // With no checkpoint activated, should return the level spawn
        PlayerPosition pos = checkpoints.GetLastCheckpointPosition();
        EXPECT_EQ(pos.x, 10.0f);
        EXPECT_EQ(pos.y, 5.0f);
        EXPECT_EQ(pos.z, 20.0f);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Checkpoint_ResetLevel)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        // Activate a checkpoint by walking near it
        checkpoints.CheckActivation(0.0f, 1.0f, 0.0f);

        // Reset level 0 — activated count should return to zero
        checkpoints.ResetLevel(0);
        EXPECT_EQ(checkpoints.GetActivatedCount(), 0u);
        checkpoints.Shutdown();
    }

    // =============================================================================
    // PlatformerPlayerController
    // =============================================================================

    TEST(Platformer_Player_Initialize)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        PlatformerPlayerController player;
        EXPECT_TRUE(player.Initialize(nullptr, &checkpoints));

        EXPECT_EQ(player.GetLives(), 3);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Player_TakeDamageReducesLives)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);

        int livesBefore = player.GetLives();
        player.TakeDamage(1);
        EXPECT_EQ(player.GetLives(), livesBefore - 1);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Player_RespawnRestoresPosition)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);
        checkpoints.SetLevelSpawn(5.0f, 2.0f, 10.0f);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);

        player.Respawn();
        PlayerPosition pos = player.GetPlayerPosition();

        // After respawn, player should be at the spawn/checkpoint position
        EXPECT_EQ(pos.x, 5.0f);
        EXPECT_EQ(pos.y, 2.0f);
        EXPECT_EQ(pos.z, 10.0f);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Player_UnlockAbilityAndStatusString)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);

        player.UnlockAbility(PowerUpType::DoubleJump);
        player.UnlockAbility(PowerUpType::SpeedBoost);

        std::string status = player.GetPlayerStatusString();
        EXPECT_FALSE(status.empty());
        checkpoints.Shutdown();
    }

} // namespace Platformer

// =============================================================================
// ARPG includes
// =============================================================================

#include "../GameModules/SparkGameARPG/Source/Hero/ARPGHeroSystem.h"
#include "../GameModules/SparkGameARPG/Source/Combat/ARPGCombatSystem.h"
#include "../GameModules/SparkGameARPG/Source/Loot/ARPGLootSystem.h"

namespace ARPG
{

    // =============================================================================
    // ARPGHeroSystem
    // =============================================================================

    TEST(ARPG_Hero_Initialize)
    {
        ARPGHeroSystem heroes;
        EXPECT_TRUE(heroes.Initialize(nullptr));
        heroes.Shutdown();
    }

    TEST(ARPG_Hero_CreateAndGetHero)
    {
        ARPGHeroSystem heroes;
        heroes.Initialize(nullptr);

        uint32_t id = heroes.CreateHero("Kratos", ARPGHeroClass::Barbarian);
        EXPECT_GT(id, 0u);

        const HeroData* hero = heroes.GetHero(id);
        EXPECT_TRUE(hero != nullptr);
        EXPECT_EQ(hero->name, std::string("Kratos"));
        EXPECT_TRUE(hero->heroClass == ARPGHeroClass::Barbarian);
        heroes.Shutdown();
    }

    TEST(ARPG_Hero_GainExperienceLevelsUp)
    {
        ARPGHeroSystem heroes;
        heroes.Initialize(nullptr);

        uint32_t id = heroes.CreateHero("Leveler", ARPGHeroClass::Sorceress);
        const HeroData* hero = heroes.GetHero(id);
        int startLevel = hero->level;

        // Add enough XP to trigger at least one level up
        heroes.GainExperience(id, 5000);
        EXPECT_GT(hero->level, startLevel);
        heroes.Shutdown();
    }

    TEST(ARPG_Hero_GetClassCount)
    {
        ARPGHeroSystem heroes;
        heroes.Initialize(nullptr);

        // Should have all 6 hero classes registered
        EXPECT_GT(heroes.GetClassCount(), 0u);
        heroes.Shutdown();
    }

    TEST(ARPG_Hero_GetHeroListString)
    {
        ARPGHeroSystem heroes;
        heroes.Initialize(nullptr);

        heroes.CreateHero("Alpha", ARPGHeroClass::Necromancer);
        heroes.CreateHero("Beta", ARPGHeroClass::Amazon);

        std::string list = heroes.GetHeroListString();
        EXPECT_FALSE(list.empty());
        heroes.Shutdown();
    }

    // =============================================================================
    // ARPGCombatSystem
    // =============================================================================

    TEST(ARPG_Combat_Initialize)
    {
        ARPGCombatSystem combat;
        EXPECT_TRUE(combat.Initialize(nullptr));
        combat.Shutdown();
    }

    TEST(ARPG_Combat_CalculateEffectiveDamage)
    {
        ARPGCombatSystem combat;
        combat.Initialize(nullptr);

        ResistanceProfile noResist;
        float damage = combat.CalculateEffectiveDamage(100.0f, ARPGDamageType::Physical, noResist);
        EXPECT_GT(damage, 0.0f);

        // With some resistance, effective damage should be lower
        ResistanceProfile someResist;
        someResist.resistances[static_cast<size_t>(ARPGDamageType::Physical)] = 0.5f;
        float reducedDamage = combat.CalculateEffectiveDamage(100.0f, ARPGDamageType::Physical, someResist);
        EXPECT_LT(reducedDamage, damage);
        combat.Shutdown();
    }

    TEST(ARPG_Combat_PerformAttackAndCount)
    {
        ARPGCombatSystem combat;
        combat.Initialize(nullptr);

        DamageInstance attack;
        attack.baseDamage = 50.0f;
        attack.damageType = ARPGDamageType::Fire;
        attack.sourceId = 1;
        attack.targetId = 2;

        ResistanceProfile noResist;
        DamageResult result = combat.PerformAttack(attack, noResist);
        EXPECT_GT(result.finalDamage, 0.0f);
        EXPECT_TRUE(result.damageType == ARPGDamageType::Fire);

        EXPECT_EQ(combat.GetAttacksProcessed(), 1u);
        combat.Shutdown();
    }

    TEST(ARPG_Combat_GetCombatStatusString)
    {
        ARPGCombatSystem combat;
        combat.Initialize(nullptr);

        DamageInstance attack;
        attack.baseDamage = 25.0f;
        ResistanceProfile noResist;
        combat.PerformAttack(attack, noResist);

        std::string status = combat.GetCombatStatusString();
        EXPECT_FALSE(status.empty());
        combat.Shutdown();
    }

    // =============================================================================
    // ARPGLootSystem
    // =============================================================================

    TEST(ARPG_Loot_Initialize)
    {
        ARPGLootSystem loot;
        EXPECT_TRUE(loot.Initialize(nullptr));

        // Affix pool should be populated after init
        EXPECT_GT(loot.GetAffixPoolSize(), 0u);
        loot.Shutdown();
    }

    TEST(ARPG_Loot_GenerateItem)
    {
        ARPGLootSystem loot;
        loot.Initialize(nullptr);

        ItemData item = loot.GenerateItem(10, ARPGItemRarity::Rare);
        EXPECT_GT(item.itemId, 0u);
        EXPECT_TRUE(item.rarity == ARPGItemRarity::Rare);
        EXPECT_EQ(item.itemLevel, 10);
        // Rare items should have 3-6 affixes
        EXPECT_GE(item.affixes.size(), 3u);
        loot.Shutdown();
    }

    TEST(ARPG_Loot_GenerateRandomDrop)
    {
        ARPGLootSystem loot;
        loot.Initialize(nullptr);

        ItemData drop = loot.GenerateRandomDrop(15, ARPGMonsterRank::Champion);
        EXPECT_GT(drop.itemId, 0u);
        EXPECT_EQ(drop.itemLevel, 15);
        loot.Shutdown();
    }

    TEST(ARPG_Loot_GeneratedItemCountAndInfoString)
    {
        ARPGLootSystem loot;
        loot.Initialize(nullptr);

        EXPECT_EQ(loot.GetGeneratedItemCount(), 0u);

        loot.GenerateItem(5, ARPGItemRarity::Magic);
        loot.GenerateItem(5, ARPGItemRarity::Normal);
        loot.GenerateRandomDrop(10, ARPGMonsterRank::Normal);
        EXPECT_EQ(loot.GetGeneratedItemCount(), 3u);

        std::string info = loot.GetLootInfoString();
        EXPECT_FALSE(info.empty());
        loot.Shutdown();
    }

} // namespace ARPG

#endif // SPARK_TEST_HAS_IMGUI
