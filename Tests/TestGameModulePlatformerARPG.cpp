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

    TEST(Platformer_Player_DeterministicMovementAcceleratesAndRuns)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);
        checkpoints.SetLevelSpawn(0.0f, 0.0f, 0.0f);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);
        player.Respawn();
        player.SetMovementInput(1.0f);
        player.FixedUpdate(0.1f);

        EXPECT_NEAR(player.GetPlayerVelocity().x, 4.0f, 0.001f);
        EXPECT_NEAR(player.GetPlayerPosition().x, 0.4f, 0.001f);

        player.SetMovementInput(1.0f, true);
        for (int i = 0; i < 10; ++i)
            player.FixedUpdate(0.1f);
        EXPECT_NEAR(player.GetPlayerVelocity().x, 12.0f, 0.001f);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Player_JumpRequestSurvivesOneFullBufferInterval)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);
        checkpoints.SetLevelSpawn(0.0f, 0.0f, 0.0f);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);
        player.Respawn();
        player.SetJumpInput(true);
        player.FixedUpdate(0.1f);

        EXPECT_TRUE(player.GetPlayerVelocity().y > 0.0f);
        EXPECT_EQ(player.GetStateString(), std::string("Jumping"));
        checkpoints.Shutdown();
    }

    TEST(Platformer_Player_DashUsesFacingDirectionAfterUnlock)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);
        checkpoints.SetLevelSpawn(0.0f, 0.0f, 0.0f);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);
        player.Respawn();
        player.UnlockAbility(PowerUpType::Dash);
        player.SetMovementInput(-1.0f);
        player.RequestDash();
        player.FixedUpdate(0.05f);

        EXPECT_EQ(player.GetStateString(), std::string("Dashing"));
        EXPECT_TRUE(player.GetPlayerPosition().x < -0.9f);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Player_DamageRejectsInvalidAmountsAndHonorsInvincibility)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);

        PlatformerPlayerController player;
        player.Initialize(nullptr, &checkpoints);
        EXPECT_FALSE(player.TakeDamage(0));
        EXPECT_FALSE(player.TakeDamage(-3));
        EXPECT_EQ(player.GetLives(), 3);

        EXPECT_TRUE(player.TakeDamage(1));
        EXPECT_EQ(player.GetLives(), 2);
        EXPECT_FALSE(player.TakeDamage(1));
        EXPECT_EQ(player.GetLives(), 2);

        player.Update(-1.0f);
        EXPECT_FALSE(player.TakeDamage(1));
        EXPECT_EQ(player.GetLives(), 2);

        player.Update(2.0f);
        EXPECT_TRUE(player.TakeDamage(1));
        EXPECT_EQ(player.GetLives(), 1);
        player.GrantLives(100);
        EXPECT_EQ(player.GetLives(), 9);
        checkpoints.Shutdown();
    }

    TEST(Platformer_Checkpoint_ActivationIsScopedToActiveLevel)
    {
        PlatformerCheckpointSystem checkpoints;
        checkpoints.Initialize(nullptr);
        checkpoints.SetActiveLevel(1);

        checkpoints.CheckActivation(25.0f, 5.0f, 0.0f);
        EXPECT_EQ(checkpoints.GetActivatedCount(), 0u);

        checkpoints.CheckActivation(30.0f, 1.0f, 0.0f);
        EXPECT_EQ(checkpoints.GetActivatedCount(), 1u);
        const auto respawn = checkpoints.GetLastCheckpointPosition();
        EXPECT_EQ(respawn.x, 30.0f);
        EXPECT_EQ(respawn.y, 1.0f);
        checkpoints.Shutdown();
    }

} // namespace Platformer

// =============================================================================
// ARPG includes
// =============================================================================

#include "../GameModules/SparkGameARPG/Source/Hero/ARPGHeroSystem.h"
#include "../GameModules/SparkGameARPG/Source/Combat/ARPGCombatSystem.h"
#include "../GameModules/SparkGameARPG/Source/Loot/ARPGLootSystem.h"
#include "../GameModules/SparkGameARPG/Source/Dungeon/ARPGDungeonSystem.h"
#include "../GameModules/SparkGameARPG/Source/Skill/ARPGSkillSystem.h"
#include "../GameModules/SparkGameARPG/Source/Monster/ARPGMonsterSystem.h"
#include "../GameModules/SparkGameARPG/Source/Demo/ARPGDemoEncounter.h"
#include "../GameModules/SparkGameARPG/Source/Core/ARPGAbilityCatalog.h"
#include "../GameModules/SparkGameARPG/Source/Core/ARPGEngineSystems.h"

#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Gameplay/AbilitySystem.h"

#include <limits>

namespace ARPG
{

    namespace
    {
        class ARPGEngineTestContext final : public Spark::IEngineContext
        {
          public:
            explicit ARPGEngineTestContext(bool exposeCoroutine = true) : m_exposeCoroutine(exposeCoroutine) {}

            GraphicsEngine* GetGraphics() override { return nullptr; }
            const GraphicsEngine* GetGraphics() const override { return nullptr; }
            InputManager* GetInput() override { return nullptr; }
            const InputManager* GetInput() const override { return nullptr; }
            Timer* GetTimer() override { return nullptr; }
            const Timer* GetTimer() const override { return nullptr; }
            Spark::EventBus* GetEventBus() override { return nullptr; }
            const Spark::EventBus* GetEventBus() const override { return nullptr; }
            AudioEngine* GetAudio() override { return nullptr; }
            const AudioEngine* GetAudio() const override { return nullptr; }
            PhysicsSystem* GetPhysics() override { return nullptr; }
            const PhysicsSystem* GetPhysics() const override { return nullptr; }
            Spark::Animation::AnimationSystem* GetAnimation() override
            {
                return &Spark::Animation::AnimationManager::GetInstance();
            }
            const Spark::Animation::AnimationSystem* GetAnimation() const override
            {
                return &Spark::Animation::AnimationManager::GetInstance();
            }
            Spark::CoroutineScheduler* GetCoroutineScheduler() override
            {
                return m_exposeCoroutine ? &Spark::CoroutineScheduler::GetInstance() : nullptr;
            }
            const Spark::CoroutineScheduler* GetCoroutineScheduler() const override
            {
                return m_exposeCoroutine ? &Spark::CoroutineScheduler::GetInstance() : nullptr;
            }
            Spark::Gameplay::AbilitySystem* GetAbilities() override
            {
                return &Spark::Gameplay::AbilitySystem::GetInstance();
            }
            const Spark::Gameplay::AbilitySystem* GetAbilities() const override
            {
                return &Spark::Gameplay::AbilitySystem::GetInstance();
            }
            uint32_t GetEngineVersion() const override { return 0; }
            uint32_t GetSDKVersion() const override { return 0; }

          private:
            bool m_exposeCoroutine = true;
        };

        struct ARPGEngineFixture
        {
            ARPGHeroSystem heroes;
            ARPGCombatSystem combat;
            ARPGLootSystem loot;
            ARPGDungeonSystem dungeon;

            void Initialize()
            {
                heroes.Initialize(nullptr);
                combat.Initialize(nullptr);
                loot.Initialize(nullptr);
                dungeon.Initialize(nullptr);
            }

            void Shutdown()
            {
                dungeon.Shutdown();
                loot.Shutdown();
                combat.Shutdown();
                heroes.Shutdown();
            }
        };
    } // namespace

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
        ASSERT_TRUE(hero != nullptr);
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

    TEST(ARPG_Hero_RegenerationIsDeterministicAndClamped)
    {
        ARPGHeroSystem heroes;
        heroes.Initialize(nullptr);

        const uint32_t id = heroes.CreateHero("Regenerator", ARPGHeroClass::Sorceress);
        HeroData* hero = heroes.GetHero(id);
        EXPECT_TRUE(hero != nullptr);
        if (!hero)
        {
            heroes.Shutdown();
            return;
        }
        hero->health = 50.0f;
        hero->mana = 25.0f;

        heroes.Update(2.0f);
        EXPECT_NEAR(hero->health, 52.0f, 0.001f);
        EXPECT_NEAR(hero->mana, 40.0f, 0.001f);

        heroes.Update(1000.0f);
        EXPECT_NEAR(hero->health, hero->maxHealth, 0.001f);
        EXPECT_NEAR(hero->mana, hero->maxMana, 0.001f);

        const float fullHealth = hero->health;
        const float fullMana = hero->mana;
        heroes.Update(-1.0f);
        heroes.Update(std::numeric_limits<float>::quiet_NaN());
        EXPECT_NEAR(hero->health, fullHealth, 0.001f);
        EXPECT_NEAR(hero->mana, fullMana, 0.001f);
        heroes.Shutdown();
    }

    TEST(ARPG_EngineBridge_RegistersAbilitiesAurasAndAnimationClips)
    {
        auto& abilities = Spark::Gameplay::AbilitySystem::GetInstance();
        abilities.Shutdown();
        abilities.Initialize(nullptr);

        ARPGEngineFixture fixture;
        fixture.Initialize();
        ARPGEngineTestContext context;
        ARPGEngineSystems bridge;
        EXPECT_TRUE(bridge.Initialize(&context, &fixture.heroes, &fixture.combat, &fixture.loot, &fixture.dungeon));

        EXPECT_TRUE(bridge.HasEngineAbilityBridge());
        EXPECT_TRUE(bridge.HasEngineAnimationBridge());
        EXPECT_EQ(bridge.GetRegisteredAbilityCount(), 4u);
        EXPECT_EQ(bridge.GetRegisteredAuraCount(), 4u);
        EXPECT_EQ(bridge.GetRegisteredProcCount(), 1u);

        const auto* fireball = abilities.GetAbilityDef(ARPGAbilityCatalog::FIREBALL_ABILITY_ID);
        EXPECT_TRUE(fireball != nullptr);
        if (fireball)
        {
            EXPECT_EQ(fireball->name, std::string("ARPG Fireball"));
            EXPECT_EQ(fireball->effects.size(), 1u);
            if (!fireball->effects.empty())
                EXPECT_TRUE(fireball->effects.front().school == Spark::Gameplay::AbilitySchool::Fire);
        }

        const auto* poison = abilities.GetAuraDef(ARPGAbilityCatalog::POISON_AURA_ID);
        EXPECT_TRUE(poison != nullptr);
        if (poison)
        {
            EXPECT_TRUE(poison->type == Spark::Gameplay::AuraType::DamageOverTime);
            EXPECT_EQ(poison->maxStacks, 3);
        }

        const auto attackClip = Spark::Animation::AnimationManager::GetInstance().GetClip("arpg_hero_attack");
        EXPECT_TRUE(attackClip != nullptr);
        if (attackClip)
            EXPECT_FALSE(attackClip->loop);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Idle"));

        bridge.Shutdown();
        fixture.Shutdown();
        abilities.Shutdown();
    }

    TEST(ARPG_EngineBridge_RejectsMissingGameplayDependencies)
    {
        ARPGEngineFixture fixture;
        fixture.Initialize();
        ARPGEngineTestContext context;
        ARPGEngineSystems bridge;

        EXPECT_FALSE(bridge.Initialize(nullptr, &fixture.heroes, &fixture.combat, &fixture.loot, &fixture.dungeon));
        EXPECT_FALSE(bridge.Initialize(&context, nullptr, &fixture.combat, &fixture.loot, &fixture.dungeon));
        EXPECT_FALSE(bridge.Initialize(&context, &fixture.heroes, nullptr, &fixture.loot, &fixture.dungeon));
        EXPECT_FALSE(bridge.Initialize(&context, &fixture.heroes, &fixture.combat, nullptr, &fixture.dungeon));
        EXPECT_FALSE(bridge.Initialize(&context, &fixture.heroes, &fixture.combat, &fixture.loot, nullptr));

        fixture.Shutdown();
    }

    TEST(ARPG_EngineBridge_ActionRecoveryUsesEngineCoroutineAndSupersedesOldAction)
    {
        auto& scheduler = Spark::CoroutineScheduler::GetInstance();
        scheduler.StopAll();
        scheduler.Update(0.0f);

        ARPGEngineFixture fixture;
        fixture.Initialize();
        ARPGEngineTestContext context;
        ARPGEngineSystems bridge;
        EXPECT_TRUE(bridge.Initialize(&context, &fixture.heroes, &fixture.combat, &fixture.loot, &fixture.dungeon));

        bridge.PlayHeroAction(ARPGHeroAction::BasicAttack);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Attack"));
        EXPECT_TRUE(bridge.IsHeroActionActive());
        scheduler.Update(0.20f);

        bridge.PlayHeroAction(ARPGHeroAction::Cast);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Cast"));
        scheduler.Update(0.44f);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Cast"));
        scheduler.Update(0.22f);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Idle"));
        EXPECT_FALSE(bridge.IsHeroActionActive());

        bridge.Shutdown();
        scheduler.Update(0.0f);
        fixture.Shutdown();
        Spark::Gameplay::AbilitySystem::GetInstance().Shutdown();
    }

    TEST(ARPG_EngineBridge_ActionRecoveryFallsBackWithoutScheduler)
    {
        ARPGEngineFixture fixture;
        fixture.Initialize();
        ARPGEngineTestContext context(false);
        ARPGEngineSystems bridge;
        EXPECT_TRUE(bridge.Initialize(&context, &fixture.heroes, &fixture.combat, &fixture.loot, &fixture.dungeon));

        bridge.PlayHeroAction(ARPGHeroAction::BasicAttack);
        bridge.Update(0.44f);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Attack"));
        bridge.Update(0.02f);
        EXPECT_EQ(bridge.GetHeroAnimationState(), std::string("Idle"));

        bridge.Shutdown();
        fixture.Shutdown();
        Spark::Gameplay::AbilitySystem::GetInstance().Shutdown();
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

    // =============================================================================
    // Skills, monsters, and playable encounter
    // =============================================================================

    TEST(ARPG_Skill_UseRequiresLearningAndHonorsCooldown)
    {
        ARPGHeroSystem heroes;
        ARPGSkillSystem skills;
        heroes.Initialize(nullptr);
        EXPECT_TRUE(skills.Initialize(nullptr, &heroes));

        const auto available = skills.GetAvailableSkills(ARPGHeroClass::Sorceress, 1);
        ASSERT_FALSE(available.empty());
        ASSERT_TRUE(available.front() != nullptr);
        const uint32_t skillId = available.front()->skillId;
        const uint32_t sorceress = heroes.CreateHero("Test Sorceress", ARPGHeroClass::Sorceress);
        const uint32_t barbarian = heroes.CreateHero("Test Barbarian", ARPGHeroClass::Barbarian);

        EXPECT_FALSE(skills.UseSkill(42, skillId));
        EXPECT_FALSE(skills.LearnSkill(42, skillId));
        EXPECT_FALSE(skills.LearnSkill(barbarian, skillId));
        EXPECT_TRUE(skills.LearnSkill(sorceress, skillId));
        EXPECT_TRUE(skills.UseSkill(sorceress, skillId));
        if (available.front()->cooldown > 0.0f)
        {
            EXPECT_FALSE(skills.UseSkill(sorceress, skillId));
            skills.Update(available.front()->cooldown);
            EXPECT_TRUE(skills.UseSkill(sorceress, skillId));
        }
        skills.Shutdown();
        heroes.Shutdown();
    }

    TEST(ARPG_Monster_BossStoredStateMatchesSpawnResult)
    {
        ARPGMonsterSystem monsters;
        monsters.Initialize(nullptr);

        const MonsterData boss = monsters.SpawnBoss(5);
        const MonsterData* stored = monsters.GetMonster(boss.monsterId);
        ASSERT_TRUE(stored != nullptr);
        EXPECT_EQ(stored->name, boss.name);
        EXPECT_TRUE(stored->rank == ARPGMonsterRank::Boss);
        EXPECT_EQ(stored->affixes.size(), boss.affixes.size());

        monsters.Shutdown();
    }

    TEST(ARPG_Monster_DamageRejectsInvalidAmountsAndRemovesDefeated)
    {
        ARPGMonsterSystem monsters;
        monsters.Initialize(nullptr);

        const MonsterData monster = monsters.SpawnMonster("Skeleton", 1);
        EXPECT_FALSE(monsters.DamageMonster(monster.monsterId, -1.0f));
        EXPECT_FALSE(monsters.DamageMonster(monster.monsterId, std::numeric_limits<float>::quiet_NaN()));
        EXPECT_TRUE(monsters.DamageMonster(monster.monsterId, monster.maxHealth + 1.0f));
        monsters.Update(0.0f);
        EXPECT_TRUE(monsters.GetMonster(monster.monsterId) == nullptr);

        monsters.Shutdown();
    }

    TEST(ARPG_DemoEncounter_AdvancesFloorAndRestarts)
    {
        ARPGHeroSystem heroes;
        ARPGCombatSystem combat;
        ARPGLootSystem loot;
        ARPGDungeonSystem dungeon;
        ARPGSkillSystem skills;
        ARPGMonsterSystem monsters;
        heroes.Initialize(nullptr);
        combat.Initialize(nullptr);
        loot.Initialize(nullptr);
        dungeon.Initialize(nullptr);
        skills.Initialize(nullptr, &heroes);
        monsters.Initialize(nullptr);

        ARPGDemoEncounter encounter;
        EXPECT_TRUE(encounter.Initialize(&heroes, &combat, &loot, &dungeon, &skills, &monsters));
        EXPECT_EQ(dungeon.GetCurrentFloorNumber(), 1);
        EXPECT_TRUE(encounter.GetTarget() != nullptr);

        for (int attack = 0; attack < 20 && encounter.GetState().totalKills < 3; ++attack)
            EXPECT_TRUE(encounter.BasicAttack());

        EXPECT_EQ(encounter.GetState().totalKills, 3u);
        EXPECT_EQ(dungeon.GetCurrentFloorNumber(), 2);
        EXPECT_EQ(loot.GetGeneratedItemCount(), 3u);
        EXPECT_TRUE(encounter.GetTarget() != nullptr);

        encounter.Restart();
        EXPECT_EQ(encounter.GetState().totalKills, 0u);
        EXPECT_EQ(dungeon.GetCurrentFloorNumber(), 1);
        EXPECT_TRUE(encounter.GetTarget() != nullptr);
        EXPECT_NEAR(encounter.GetHero()->health, encounter.GetHero()->maxHealth, 0.001f);
        EXPECT_NEAR(encounter.GetHero()->mana, encounter.GetHero()->maxMana, 0.001f);

        encounter.Shutdown();
        monsters.Shutdown();
        skills.Shutdown();
        dungeon.Shutdown();
        loot.Shutdown();
        combat.Shutdown();
        heroes.Shutdown();
    }

    TEST(ARPG_DemoEncounter_ReconcilesExternallyDefeatedTarget)
    {
        ARPGHeroSystem heroes;
        ARPGCombatSystem combat;
        ARPGLootSystem loot;
        ARPGDungeonSystem dungeon;
        ARPGSkillSystem skills;
        ARPGMonsterSystem monsters;
        heroes.Initialize(nullptr);
        combat.Initialize(nullptr);
        loot.Initialize(nullptr);
        dungeon.Initialize(nullptr);
        skills.Initialize(nullptr, &heroes);
        monsters.Initialize(nullptr);

        ARPGDemoEncounter encounter;
        EXPECT_TRUE(encounter.Initialize(&heroes, &combat, &loot, &dungeon, &skills, &monsters));
        const MonsterData* originalTarget = encounter.GetTarget();
        EXPECT_TRUE(originalTarget != nullptr);
        const uint32_t originalTargetId = originalTarget ? originalTarget->monsterId : 0;
        const float lethalDamage = originalTarget ? originalTarget->maxHealth + 1.0f : 1.0f;

        EXPECT_TRUE(monsters.DamageMonster(originalTargetId, lethalDamage));
        encounter.Update();

        EXPECT_EQ(encounter.GetState().totalKills, 1u);
        EXPECT_TRUE(encounter.GetTarget() != nullptr);
        EXPECT_NE(encounter.GetState().targetMonsterId, originalTargetId);

        encounter.Shutdown();
        monsters.Shutdown();
        skills.Shutdown();
        dungeon.Shutdown();
        loot.Shutdown();
        combat.Shutdown();
        heroes.Shutdown();
    }

    TEST(ARPG_DemoEncounter_StateRoundTripRestoresProgress)
    {
        ARPGHeroSystem heroes;
        ARPGCombatSystem combat;
        ARPGLootSystem loot;
        ARPGDungeonSystem dungeon;
        ARPGSkillSystem skills;
        ARPGMonsterSystem monsters;
        heroes.Initialize(nullptr);
        combat.Initialize(nullptr);
        loot.Initialize(nullptr);
        dungeon.Initialize(nullptr);
        skills.Initialize(nullptr, &heroes);
        monsters.Initialize(nullptr);

        ARPGDemoEncounter encounter;
        EXPECT_TRUE(encounter.Initialize(&heroes, &combat, &loot, &dungeon, &skills, &monsters));
        for (int attack = 0; attack < 20 && encounter.GetState().totalKills < 3; ++attack)
            EXPECT_TRUE(encounter.BasicAttack());
        EXPECT_EQ(dungeon.GetCurrentFloorNumber(), 2);
        EXPECT_TRUE(encounter.BasicAttack());

        const std::string snapshot = encounter.SerializeState();
        const uint32_t savedKills = encounter.GetState().totalKills;
        const float savedTargetHealth = encounter.GetTarget()->health;
        const int savedAttributePoints = heroes.GetHero(encounter.GetState().heroId)->freeAttributePoints;
        const float savedMoveSpeed = heroes.GetHero(encounter.GetState().heroId)->moveSpeed;
        EXPECT_TRUE(encounter.CanRestoreState(snapshot));
        EXPECT_FALSE(encounter.CanRestoreState(snapshot + " trailing-garbage"));
        encounter.Restart();
        heroes.GetHero(encounter.GetState().heroId)->freeAttributePoints = 999;
        heroes.GetHero(encounter.GetState().heroId)->moveSpeed = 99.0f;
        EXPECT_EQ(dungeon.GetCurrentFloorNumber(), 1);

        EXPECT_TRUE(encounter.RestoreState(snapshot));
        EXPECT_EQ(dungeon.GetCurrentFloorNumber(), 2);
        EXPECT_EQ(encounter.GetState().totalKills, savedKills);
        EXPECT_NEAR(encounter.GetTarget()->health, savedTargetHealth, 0.001f);
        EXPECT_EQ(heroes.GetHero(encounter.GetState().heroId)->freeAttributePoints, savedAttributePoints);
        EXPECT_NEAR(heroes.GetHero(encounter.GetState().heroId)->moveSpeed, savedMoveSpeed, 0.001f);
        EXPECT_EQ(encounter.SerializeState(), snapshot);
        EXPECT_FALSE(encounter.RestoreState("ARPGDEMO 99 corrupt"));

        encounter.Shutdown();
        monsters.Shutdown();
        skills.Shutdown();
        dungeon.Shutdown();
        loot.Shutdown();
        combat.Shutdown();
        heroes.Shutdown();
    }

} // namespace ARPG

#endif // SPARK_TEST_HAS_IMGUI
