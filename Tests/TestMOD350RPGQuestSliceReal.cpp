/**
 * @file TestMOD350RPGQuestSliceReal.cpp
 * @brief MOD-350: the RPG quest chain completes end to end through the real RPGDemoSession API
 *
 * Every test drives the production SparkGameRPG sources (character, combat, inventory, NPC, world,
 * demo session) with the real RPGGameplayBridge installed as the engine QuestSystem policy, exactly
 * as SparkGameRPGModule wires them. Input goes through the session entry points the rpg_talk,
 * rpg_accept, rpg_travel, rpg_attack, rpg_flee and rpg_rest console commands call; the combat
 * system is ticked between swings the way the module's update loop ticks it. Critical hits come
 * from an unseeded RNG, so fights loop until the encounter ends instead of asserting hit counts.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRPG/Source/Character/RPGCharacterSystem.h"
#include "../GameModules/SparkGameRPG/Source/Combat/RPGCombatSystem.h"
#include "../GameModules/SparkGameRPG/Source/Gameplay/RPGDemoSession.h"
#include "../GameModules/SparkGameRPG/Source/Gameplay/RPGGameplayBridge.h"
#include "../GameModules/SparkGameRPG/Source/Inventory/RPGInventorySystem.h"
#include "../GameModules/SparkGameRPG/Source/NPC/RPGNPCSystem.h"
#include "../GameModules/SparkGameRPG/Source/World/RPGWorldSetup.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Gameplay/QuestSystem.h"
#include "Spark/IEngineContext.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace RPG;
using EngineQuestState = Spark::Gameplay::QuestState; // RPG:: has its own legacy QuestState
using Spark::Gameplay::QuestSystem;

namespace
{
    constexpr uint32_t kVillageArea = 1;
    constexpr uint32_t kForestArea = 2;
    constexpr uint32_t kElderNpc = 2;  // Elder Mirwen, offers the wolf hunt (quest 1)
    constexpr uint32_t kHermitNpc = 5; // Old Theron in Thornwood, target of quest 3
    constexpr uint32_t kWolfHuntQuest = 1;
    constexpr uint32_t kHerbQuest = 2;
    constexpr uint32_t kHermitQuest = 3;
    constexpr uint32_t kHealthPotion = 1;
    constexpr uint32_t kManaElixir = 2;
    constexpr uint32_t kMoonpetalHerb = 200;

    /// Engine context that exposes only a real DialogueSystem, the one subsystem the bridge consumes.
    class RPGQuestSliceContext final : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        Spark::DialogueSystem* GetDialogue() override { return &m_dialogue; }
        const Spark::DialogueSystem* GetDialogue() const override { return &m_dialogue; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

      private:
        Spark::DialogueSystem m_dialogue;
    };

    /// The RPG module's gameplay stack, initialized and torn down in SparkGameRPGModule order.
    struct RPGQuestSlice
    {
        RPGQuestSliceContext context;
        RPGCharacterSystem characters;
        RPGCombatSystem combat;
        RPGInventorySystem inventory;
        RPGNPCSystem npcs;
        RPGWorldSetup world;
        RPGGameplayBridge bridge;
        RPGDemoSession session;
        bool initialized = false;

        RPGQuestSlice()
        {
            QuestSystem::GetInstance().Initialize();
            initialized = characters.Initialize(nullptr) && combat.Initialize(nullptr) &&
                          inventory.Initialize(nullptr) && npcs.Initialize(nullptr) && world.Initialize(nullptr) &&
                          bridge.Initialize(&context, &characters) &&
                          session.Initialize(&characters, &combat, &inventory, &npcs, &world);
        }

        ~RPGQuestSlice()
        {
            session.Shutdown();
            bridge.Shutdown();
            world.Shutdown();
            npcs.Shutdown();
            inventory.Shutdown();
            combat.Shutdown();
            characters.Shutdown();
            QuestSystem::GetInstance().Shutdown();
        }

        RPGQuestSlice(const RPGQuestSlice&) = delete;
        RPGQuestSlice& operator=(const RPGQuestSlice&) = delete;

        [[nodiscard]] uint32_t Player() const { return session.GetPlayerCharacterId(); }
        [[nodiscard]] const CharacterData* Hero() const { return characters.GetCharacter(Player()); }

        [[nodiscard]] int CountItem(uint32_t itemId) const
        {
            int total = 0;
            for (const RPGItemStack& stack : session.GetPlayerInventory().slots)
            {
                if (stack.itemDefId == itemId)
                    total += stack.count;
            }
            return total;
        }

        [[nodiscard]] uint32_t ObjectiveCount(uint32_t questId) const
        {
            for (const auto& quest : QuestSystem::GetInstance().CaptureEntityState(Player()))
            {
                if (quest.questId == questId && !quest.objectiveCounts.empty())
                    return quest.objectiveCounts[0];
            }
            return 0;
        }

        /// Swing until the current encounter ends. Returns false if Rowan falls or the fight stalls.
        bool FightCurrentEncounter()
        {
            for (int swing = 0; swing < 64 && session.IsInCombat(); ++swing)
            {
                combat.Update(30.0f); // one module frame long enough to clear any ability cooldown
                const std::string result = session.Attack();
                if (result.find("Rowan was defeated") != std::string::npos)
                    return false;
            }
            return !session.IsInCombat();
        }

        /// Rest in Oakhollow, walk into Thornwood and return the encounter line the session reports.
        std::string EnterForestRested()
        {
            if (session.GetCurrentAreaId() != kVillageArea)
                session.Travel(kVillageArea);
            session.Rest();
            return session.Travel(kForestArea);
        }

        /// Hunt in Thornwood, fleeing any encounter whose name does not contain @p quarry (empty fights all),
        /// until @p kills encounters were defeated. Returns false if the hunt cannot finish.
        bool Hunt(const std::string& quarry, int kills)
        {
            int defeated = 0;
            for (int trip = 0; trip < 400 && defeated < kills; ++trip)
            {
                const std::string encounter = EnterForestRested();
                if (encounter.find("Encountered") == std::string::npos)
                    return false;
                if (!quarry.empty() && encounter.find(quarry) == std::string::npos)
                {
                    session.Flee();
                    continue;
                }
                if (!FightCurrentEncounter())
                    return false;
                ++defeated;
            }
            return defeated == kills;
        }
    };
} // namespace

TEST(RPGQuestSlice_WolfHuntCompletesAndGrantsReward)
{
    RPGQuestSlice slice;
    ASSERT_TRUE(slice.initialized);
    auto& quests = QuestSystem::GetInstance();
    ASSERT_TRUE(quests.GetPolicy() == &slice.bridge);

    // The bridge registered its dialogue tree on the real DialogueSystem the context exposes.
    EXPECT_EQ(slice.bridge.GetRegisteredDialogueTreeCount(), static_cast<size_t>(1));
    EXPECT_TRUE(slice.context.GetDialogue()->StartConversation("rpg_blacksmith"));
    slice.context.GetDialogue()->EndConversation();

    // Elder Mirwen offers the wolf hunt; a fresh adventure already carries it, so accepting again is refused.
    EXPECT_TRUE(slice.session.Talk(kElderNpc).find("offers quest 1") != std::string::npos);
    EXPECT_TRUE(slice.session.AcceptQuest(kWolfHuntQuest).find("already active") != std::string::npos);
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kWolfHuntQuest) == EngineQuestState::Active);
    const auto* wolfHunt = quests.GetQuestDef(kWolfHuntQuest);
    ASSERT_TRUE(wolfHunt != nullptr);
    ASSERT_EQ(wolfHunt->itemRewards.size(), static_cast<size_t>(1));
    const uint32_t rewardItem = wolfHunt->itemRewards[0].first;
    const int rewardCount = static_cast<int>(wolfHunt->itemRewards[0].second);
    EXPECT_EQ(rewardItem, kHealthPotion);

    // Four shadow wolves: progress is tracked, the quest is not finished yet.
    ASSERT_TRUE(slice.Hunt("Shadow Wolf", 4));
    EXPECT_EQ(slice.ObjectiveCount(kWolfHuntQuest), 4u);
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kWolfHuntQuest) == EngineQuestState::Active);

    // Mirror the XP curve on a reference character so level-ups are accounted for exactly.
    const CharacterData* hero = slice.Hero();
    ASSERT_TRUE(hero != nullptr);
    const uint32_t reference = slice.characters.CreateCharacter("Reference", hero->classId);
    CharacterData* referenceHero = slice.characters.GetCharacter(reference);
    ASSERT_TRUE(referenceHero != nullptr);
    referenceHero->level = hero->level;
    referenceHero->xp = hero->xp;
    referenceHero->xpToNextLevel = hero->xpToNextLevel;

    const int potionsBefore = slice.CountItem(kHealthPotion);
    const int herbsBefore = slice.CountItem(kMoonpetalHerb);
    const int goldBefore = slice.session.GetPlayerInventory().currency;

    // The fifth wolf: kill loot (one potion, one herb) plus the quest's item and XP reward.
    ASSERT_TRUE(slice.Hunt("Shadow Wolf", 1));
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kWolfHuntQuest) == EngineQuestState::Completed);
    EXPECT_TRUE(quests.GetActiveQuests(slice.Player()).empty());
    EXPECT_EQ(slice.CountItem(kHealthPotion), potionsBefore + 1 + rewardCount);
    EXPECT_EQ(slice.CountItem(kMoonpetalHerb), herbsBefore + 1);
    EXPECT_EQ(slice.session.GetPlayerInventory().currency, goldBefore); // no quest in the chain pays gold

    slice.characters.AddXP(reference, 50);                 // kill XP from RPGDemoSession::FinishEnemy
    slice.characters.AddXP(reference, wolfHunt->xpReward); // quest XP through the bridge policy
    hero = slice.Hero();
    referenceHero = slice.characters.GetCharacter(reference);
    EXPECT_EQ(hero->level, referenceHero->level);
    EXPECT_EQ(hero->xp, referenceHero->xp);
    slice.characters.DestroyCharacter(reference);

    // A completed quest cannot be re-completed or re-accepted to farm the reward again.
    EXPECT_FALSE(quests.CompleteQuest(slice.Player(), kWolfHuntQuest));
    EXPECT_TRUE(slice.session.AcceptQuest(kWolfHuntQuest).find("unavailable") != std::string::npos);
    EXPECT_EQ(slice.CountItem(kHealthPotion), potionsBefore + 1 + rewardCount);

    // The completed chain survives the session's persisted snapshot.
    const std::string snapshot = slice.session.SerializeState();
    ASSERT_TRUE(slice.session.RestoreState(snapshot));
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kWolfHuntQuest) == EngineQuestState::Completed);
    EXPECT_EQ(slice.CountItem(kHealthPotion), potionsBefore + 1 + rewardCount);
}

TEST(RPGQuestSlice_ChainedQuestUnlocksAfterPrerequisite)
{
    RPGQuestSlice slice;
    ASSERT_TRUE(slice.initialized);
    auto& quests = QuestSystem::GetInstance();

    // Healing Herbs and The Dark Below stay locked while their prerequisites are open.
    EXPECT_TRUE(slice.session.AcceptQuest(kHerbQuest).find("unavailable") != std::string::npos);
    EXPECT_TRUE(slice.session.AcceptQuest(kHermitQuest).find("unavailable") != std::string::npos);
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kHerbQuest) == EngineQuestState::NotStarted);

    ASSERT_TRUE(slice.Hunt("Shadow Wolf", 5));
    ASSERT_TRUE(quests.GetQuestState(slice.Player(), kWolfHuntQuest) == EngineQuestState::Completed);
    ASSERT_TRUE(slice.Hero()->level >= 2);

    // Wolf hunt done: the herb quest unlocks, the hermit quest is still gated on it.
    EXPECT_TRUE(slice.session.AcceptQuest(kHerbQuest).find("Accepted quest 2") != std::string::npos);
    EXPECT_TRUE(slice.session.AcceptQuest(kHermitQuest).find("unavailable") != std::string::npos);
    EXPECT_EQ(slice.ObjectiveCount(kHerbQuest), 0u); // herbs looted before accepting do not count

    const auto* herbQuest = quests.GetQuestDef(kHerbQuest);
    ASSERT_TRUE(herbQuest != nullptr);
    ASSERT_EQ(herbQuest->itemRewards.size(), static_cast<size_t>(1));
    EXPECT_EQ(herbQuest->itemRewards[0].first, kManaElixir);
    const int elixirsBefore = slice.CountItem(kManaElixir);
    const int herbsRequired = static_cast<int>(herbQuest->objectives[0].requiredCount);

    // Every Thornwood kill yields one moonpetal herb.
    ASSERT_TRUE(slice.Hunt("", herbsRequired - 1));
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kHerbQuest) == EngineQuestState::Active);
    ASSERT_TRUE(slice.Hunt("", 1));
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kHerbQuest) == EngineQuestState::Completed);
    EXPECT_EQ(slice.CountItem(kManaElixir), elixirsBefore + static_cast<int>(herbQuest->itemRewards[0].second));

    // The prerequisite is met, but the bridge policy still gates The Dark Below on its required level.
    const auto* hermitQuest = quests.GetQuestDef(kHermitQuest);
    ASSERT_TRUE(hermitQuest != nullptr);
    for (int trip = 0; trip < 40 && static_cast<uint32_t>(slice.Hero()->level) < hermitQuest->requiredLevel; ++trip)
    {
        EXPECT_TRUE(slice.session.AcceptQuest(kHermitQuest).find("unavailable") != std::string::npos);
        ASSERT_TRUE(slice.Hunt("", 1));
    }
    ASSERT_TRUE(static_cast<uint32_t>(slice.Hero()->level) >= hermitQuest->requiredLevel);

    // The Dark Below unlocks; the hermit only counts once reached in Thornwood outside combat.
    EXPECT_TRUE(slice.session.AcceptQuest(kHermitQuest).find("Accepted quest 3") != std::string::npos);
    EXPECT_EQ(slice.session.GetCurrentAreaId(), kForestArea);
    ASSERT_FALSE(slice.session.IsInCombat());
    EXPECT_TRUE(slice.session.Talk(kHermitNpc).find("Old Theron") != std::string::npos);
    EXPECT_TRUE(quests.GetQuestState(slice.Player(), kHermitQuest) == EngineQuestState::Completed);

    const std::vector<uint32_t> completed = quests.GetCompletedQuests(slice.Player());
    EXPECT_EQ(completed.size(), static_cast<size_t>(3));
    EXPECT_TRUE(quests.GetActiveQuests(slice.Player()).empty());
}

#endif // SPARK_TEST_HAS_IMGUI
