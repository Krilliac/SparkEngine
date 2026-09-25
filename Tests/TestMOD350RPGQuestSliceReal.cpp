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
 *
 * The RPGPersistence_* tests add the module's RPGEngineSystems bridge on top of the real SaveSystem
 * singleton (rooted in a temporary directory) and rebuild the whole stack, including the engine
 * QuestSystem, between rpg_save and rpg_load exactly as a restarted process would.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRPG/Source/Character/RPGCharacterSystem.h"
#include "../GameModules/SparkGameRPG/Source/Core/RPGEngineSystems.h"
#include "../GameModules/SparkGameRPG/Source/Combat/RPGCombatSystem.h"
#include "../GameModules/SparkGameRPG/Source/Gameplay/RPGDemoSession.h"
#include "../GameModules/SparkGameRPG/Source/Gameplay/RPGGameplayBridge.h"
#include "../GameModules/SparkGameRPG/Source/Inventory/RPGInventorySystem.h"
#include "../GameModules/SparkGameRPG/Source/NPC/RPGNPCSystem.h"
#include "../GameModules/SparkGameRPG/Source/World/RPGWorldSetup.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/Gameplay/QuestSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Spark/IEngineContext.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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
    constexpr uint32_t kBlacksmithNpc = 1;
    constexpr uint32_t kGuardNpc = 3; // Captain Aldric, the only NPC with a patrol path

    /// Engine context exposing a real DialogueSystem (for the bridge) and, when given, the real
    /// SaveSystem and an ECS World (for RPGEngineSystems::SaveGame/LoadGame).
    class RPGQuestSliceContext final : public Spark::IEngineContext
    {
      public:
        RPGQuestSliceContext(Spark::SaveSystem* saveSystem = nullptr, ::World* world = nullptr)
            : m_saveSystem(saveSystem), m_world(world)
        {
        }

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
        ::World* GetWorld() override { return m_world; }
        const ::World* GetWorld() const override { return m_world; }
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

      private:
        Spark::DialogueSystem m_dialogue;
        Spark::SaveSystem* m_saveSystem = nullptr;
        ::World* m_world = nullptr;
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
        RPGEngineSystems engine;
        bool initialized = false;

        /// With a SaveSystem and World the module's engine bridge is wired as well, as in SparkGameRPGModule.
        explicit RPGQuestSlice(Spark::SaveSystem* saveSystem = nullptr, ::World* ecsWorld = nullptr)
            : context(saveSystem, ecsWorld)
        {
            QuestSystem::GetInstance().Initialize();
            initialized = characters.Initialize(nullptr) && combat.Initialize(nullptr) &&
                          inventory.Initialize(nullptr) && npcs.Initialize(nullptr) && world.Initialize(nullptr) &&
                          bridge.Initialize(&context, &characters) &&
                          session.Initialize(&characters, &combat, &inventory, &npcs, &world) &&
                          (saveSystem == nullptr || engine.Initialize(&context));
        }

        ~RPGQuestSlice()
        {
            engine.Shutdown();
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

        /// The rpg_save console command: snapshot the session into the engine slot.
        std::string SaveSlot(const std::string& slot) { return engine.SaveGame(slot, session.SerializeState()); }

        /// The rpg_load console command: validated SaveSystem load, then the session restore.
        std::string LoadSlot(const std::string& slot)
        {
            std::string demoState;
            const std::string result = engine.LoadGame(slot, demoState, [this](const std::string& candidate)
                                                       { return session.CanRestoreState(candidate); });
            if (!demoState.empty() && !session.RestoreState(demoState))
                return "RPG demo restore failed after validated world load: " + slot;
            return result;
        }
    };

    /// Temporary save root for the SaveSystem singleton; restores the previous directory afterwards.
    class ScopedMOD350SaveDirectory
    {
      public:
        explicit ScopedMOD350SaveDirectory(const char* name)
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / (std::string("spark_mod350_") + name))
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            std::filesystem::create_directories(m_directory, error);
            m_saveSystem.SetFileCache(nullptr);
            m_initialized = m_saveSystem.Initialize(m_directory.string());
        }

        ~ScopedMOD350SaveDirectory()
        {
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        ScopedMOD350SaveDirectory(const ScopedMOD350SaveDirectory&) = delete;
        ScopedMOD350SaveDirectory& operator=(const ScopedMOD350SaveDirectory&) = delete;

        [[nodiscard]] bool IsInitialized() const { return m_initialized; }
        Spark::SaveSystem& System() { return m_saveSystem; }
        [[nodiscard]] std::filesystem::path SlotPath(const std::string& slot) const
        {
            return m_directory / (slot + ".spark_save");
        }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        bool m_initialized = false;
    };

    bool StartsWithMOD350(const std::string& text, const char* prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }

    /// Whitespace tokens of an RPGDEMO snapshot taken outside combat (the quoted enemy name is then "").
    std::vector<std::string> SplitMOD350Snapshot(const std::string& snapshot)
    {
        std::istringstream stream(snapshot);
        std::vector<std::string> tokens;
        for (std::string token; stream >> token;)
            tokens.push_back(token);
        return tokens;
    }

    std::string JoinMOD350Snapshot(const std::vector<std::string>& tokens)
    {
        std::string joined;
        for (const std::string& token : tokens)
            joined += (joined.empty() ? "" : " ") + token;
        return joined;
    }

    /// Fields per NPC in the RPGDEMO 3 tail: id, disposition, behavior, x, y, z, waypoint index, wait timer.
    constexpr size_t kMOD350NpcFields = 8;
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

TEST(RPGPersistence_MidQuestSaveRestartRestoresQuestInventoryAreaAndNPCs)
{
    ScopedMOD350SaveDirectory saves("midquest");
    ASSERT_TRUE(saves.IsInitialized());
    auto& quests = QuestSystem::GetInstance();

    std::string savedSnapshot;
    int savedPotions = 0;
    int savedHerbs = 0;
    int savedLevel = 0;
    uint32_t savedXp = 0;
    int blacksmithDisposition = 0;
    int guardDisposition = 0;
    int guardWaypoint = 0;
    float savedHour = 0.0f;
    {
        World world;
        world.AddComponent<Transform>(world.CreateEntity("mod350-oakhollow-well"));
        auto slice = std::make_unique<RPGQuestSlice>(&saves.System(), &world);
        ASSERT_TRUE(slice->initialized);

        // Two of five wolves, a day's worth of NPC clock, and player actions that moved two NPCs' standing.
        ASSERT_TRUE(slice->Hunt("Shadow Wolf", 2));
        ASSERT_EQ(slice->ObjectiveCount(kWolfHuntQuest), 2u);
        slice->npcs.Update(150.0f); // 2.5 game hours: 10:30, and the guard advances along its patrol
        slice->npcs.AdjustDisposition(kBlacksmithNpc, 25);
        slice->npcs.AdjustDisposition(kGuardNpc, -35);
        ASSERT_FALSE(slice->session.IsInCombat());
        ASSERT_EQ(slice->session.GetCurrentAreaId(), kForestArea);

        savedSnapshot = slice->session.SerializeState();
        ASSERT_TRUE(StartsWithMOD350(savedSnapshot, "RPGDEMO 3 "));
        savedPotions = slice->CountItem(kHealthPotion);
        savedHerbs = slice->CountItem(kMoonpetalHerb);
        savedLevel = slice->Hero()->level;
        savedXp = slice->Hero()->xp;
        blacksmithDisposition = slice->npcs.GetNPC(kBlacksmithNpc)->dispositionValue;
        guardDisposition = slice->npcs.GetNPC(kGuardNpc)->dispositionValue;
        guardWaypoint = slice->npcs.GetNPC(kGuardNpc)->currentWaypointIndex;
        savedHour = slice->npcs.GetWorldHour();
        EXPECT_EQ(blacksmithDisposition, 85);
        EXPECT_EQ(guardDisposition, 15);
        EXPECT_NE(guardWaypoint, 0);
        EXPECT_NEAR(savedHour, 10.5f, 0.001f);

        EXPECT_TRUE(StartsWithMOD350(slice->SaveSlot("midquest"), "Saved RPG world"));
        EXPECT_TRUE(saves.System().SaveExists("midquest"));
    } // the whole stack, the ECS world and the QuestSystem's live state are torn down here

    // Restart: an empty ECS world and a freshly initialized stack start from the new-adventure defaults.
    World restartedWorld;
    auto restarted = std::make_unique<RPGQuestSlice>(&saves.System(), &restartedWorld);
    ASSERT_TRUE(restarted->initialized);
    EXPECT_TRUE(restarted->session.SerializeState() != savedSnapshot);
    EXPECT_EQ(restarted->session.GetCurrentAreaId(), kVillageArea);
    EXPECT_EQ(restarted->ObjectiveCount(kWolfHuntQuest), 0u);
    EXPECT_EQ(restarted->npcs.GetNPC(kBlacksmithNpc)->dispositionValue, 60);
    EXPECT_NEAR(restarted->npcs.GetWorldHour(), 8.0f, 0.001f);
    EXPECT_EQ(restartedWorld.GetEntityCount(), 0u);

    EXPECT_TRUE(StartsWithMOD350(restarted->LoadSlot("midquest"), "Loaded RPG world"));
    EXPECT_EQ(restarted->session.SerializeState(), savedSnapshot);
    EXPECT_EQ(restartedWorld.GetEntityCount(), 1u); // the ECS world came back through SaveSystem too
    EXPECT_EQ(restarted->session.GetCurrentAreaId(), kForestArea);
    EXPECT_FALSE(restarted->session.IsInCombat());
    EXPECT_TRUE(quests.GetQuestState(restarted->Player(), kWolfHuntQuest) == EngineQuestState::Active);
    EXPECT_EQ(restarted->ObjectiveCount(kWolfHuntQuest), 2u);
    EXPECT_EQ(restarted->CountItem(kHealthPotion), savedPotions);
    EXPECT_EQ(restarted->CountItem(kMoonpetalHerb), savedHerbs);
    EXPECT_EQ(restarted->Hero()->level, savedLevel);
    EXPECT_EQ(restarted->Hero()->xp, savedXp);
    const NPCData* blacksmith = restarted->npcs.GetNPC(kBlacksmithNpc);
    const NPCData* guard = restarted->npcs.GetNPC(kGuardNpc);
    EXPECT_EQ(blacksmith->dispositionValue, blacksmithDisposition);
    EXPECT_TRUE(blacksmith->disposition == NPCDisposition::Friendly);
    EXPECT_EQ(guard->dispositionValue, guardDisposition);
    EXPECT_TRUE(guard->disposition == NPCDisposition::Hostile);
    EXPECT_EQ(guard->currentWaypointIndex, guardWaypoint);
    EXPECT_NEAR(restarted->npcs.GetWorldHour(), savedHour, 0.0001f);

    // The restored quest keeps counting: three more wolves finish it and deliver the reward.
    const auto* wolfHunt = quests.GetQuestDef(kWolfHuntQuest);
    ASSERT_TRUE(wolfHunt != nullptr);
    ASSERT_EQ(wolfHunt->itemRewards.size(), static_cast<size_t>(1));
    const int rewardCount = static_cast<int>(wolfHunt->itemRewards[0].second);
    ASSERT_TRUE(restarted->Hunt("Shadow Wolf", 2));
    EXPECT_TRUE(quests.GetQuestState(restarted->Player(), kWolfHuntQuest) == EngineQuestState::Active);
    const int potionsBeforeLastKill = restarted->CountItem(kHealthPotion);
    ASSERT_TRUE(restarted->Hunt("Shadow Wolf", 1));
    ASSERT_TRUE(quests.GetQuestState(restarted->Player(), kWolfHuntQuest) == EngineQuestState::Completed);
    const int rewardedPotions = restarted->CountItem(kHealthPotion);
    EXPECT_EQ(rewardedPotions, potionsBeforeLastKill + 1 + rewardCount); // kill loot plus the quest reward

    // The completed quest and its reward survive a second restart through the same slot.
    ASSERT_TRUE(StartsWithMOD350(restarted->SaveSlot("midquest"), "Saved RPG world"));
    const std::string completedSnapshot = restarted->session.SerializeState();
    restarted.reset();
    World secondWorld;
    auto second = std::make_unique<RPGQuestSlice>(&saves.System(), &secondWorld);
    ASSERT_TRUE(second->initialized);
    EXPECT_TRUE(StartsWithMOD350(second->LoadSlot("midquest"), "Loaded RPG world"));
    EXPECT_EQ(second->session.SerializeState(), completedSnapshot);
    EXPECT_TRUE(quests.GetQuestState(second->Player(), kWolfHuntQuest) == EngineQuestState::Completed);
    EXPECT_EQ(second->CountItem(kHealthPotion), rewardedPotions);
    EXPECT_TRUE(second->session.AcceptQuest(kWolfHuntQuest).find("unavailable") != std::string::npos);
    EXPECT_TRUE(second->session.AcceptQuest(kHerbQuest).find("Accepted quest 2") != std::string::npos);
}

TEST(RPGPersistence_CorruptSlotLeavesSessionUnchanged)
{
    ScopedMOD350SaveDirectory saves("corrupt");
    ASSERT_TRUE(saves.IsInitialized());

    // Author a valid slot plus one slot per malformed RPGDEMO payload, all through the real SaveSystem.
    std::string validSnapshot;
    std::vector<std::pair<std::string, std::string>> rejectedPayloads;
    {
        World world;
        world.AddComponent<Transform>(world.CreateEntity("mod350-saved"));
        auto author = std::make_unique<RPGQuestSlice>(&saves.System(), &world);
        ASSERT_TRUE(author->initialized);
        ASSERT_TRUE(author->Hunt("Shadow Wolf", 1));
        author->npcs.Update(30.0f);
        author->npcs.AdjustDisposition(kBlacksmithNpc, -10);
        ASSERT_FALSE(author->session.IsInCombat());
        validSnapshot = author->session.SerializeState();
        ASSERT_TRUE(StartsWithMOD350(author->SaveSlot("valid"), "Saved RPG world"));

        const std::vector<std::string> tokens = SplitMOD350Snapshot(validSnapshot);
        const size_t npcCount = author->npcs.GetNPCCount();
        const size_t npcTail = 3 + npcCount * kMOD350NpcFields; // world time, hour, count, then NPC records
        ASSERT_TRUE(npcCount >= 2);
        ASSERT_TRUE(tokens.size() > npcTail + 2);
        ASSERT_EQ(tokens[1], std::string("3"));
        const size_t tailStart = tokens.size() - npcTail;
        const size_t firstNpc = tailStart + 3;
        const size_t secondNpc = firstNpc + kMOD350NpcFields;
        size_t guardRecord = 0;
        for (size_t record = firstNpc; record < tokens.size(); record += kMOD350NpcFields)
        {
            if (tokens[record] == std::to_string(kGuardNpc))
                guardRecord = record;
        }
        ASSERT_TRUE(guardRecord != 0);

        auto variant = [&](const char* slot, auto mutate)
        {
            std::vector<std::string> mutated = tokens;
            mutate(mutated);
            rejectedPayloads.emplace_back(slot, JoinMOD350Snapshot(mutated));
        };
        variant("truncated", [](auto& t) { t.pop_back(); });
        variant("trailing", [](auto& t) { t.push_back("7"); });
        // Version 1 is older than N-1 and fails closed (version 2 migrates; see the legacy test below).
        variant("legacy_v1",
                [&](auto& t)
                {
                    t.resize(tailStart);
                    t[1] = "1";
                });
        // A version-2 save ends after the quests, so a v3 NPC section behind a "2" label is trailing data.
        variant("relabelled_v2", [](auto& t) { t[1] = "2"; });
        variant("future_v4", [](auto& t) { t[1] = "4"; });
        variant("npc_count", [&](auto& t) { t[tailStart + 2] = std::to_string(npcCount + 1); });
        variant("npc_missing",
                [&](auto& t)
                {
                    t.resize(t.size() - kMOD350NpcFields);
                    t[tailStart + 2] = std::to_string(npcCount - 1);
                });
        variant("hour_24", [&](auto& t) { t[tailStart + 1] = "24"; });
        // Parse-rejection only: libstdc++ and MSVC refuse "nan"/"inf" float tokens before ValidateState runs.
        // RPGPersistence_NonFiniteNPCSnapshotRejectedWithoutChange covers the non-finite checks directly.
        variant("hour_nan", [&](auto& t) { t[tailStart + 1] = "nan"; });
        variant("negative_time", [&](auto& t) { t[tailStart] = "-1"; });
        variant("duplicate_npc", [&](auto& t) { t[secondNpc] = t[firstNpc]; });
        variant("unknown_npc", [&](auto& t) { t[firstNpc] = "999"; });
        variant("disposition_high", [&](auto& t) { t[firstNpc + 1] = "101"; });
        variant("disposition_low", [&](auto& t) { t[firstNpc + 1] = "-1"; });
        variant("behavior", [&](auto& t) { t[firstNpc + 2] = "5"; });
        variant("position_inf", [&](auto& t) { t[firstNpc + 3] = "inf"; });
        variant("waypoint_range", [&](auto& t) { t[guardRecord + 6] = "4"; });
        variant("waypoint_negative", [&](auto& t) { t[guardRecord + 6] = "-1"; });
        variant("wait_negative", [&](auto& t) { t[guardRecord + 7] = "-0.5"; });
        for (const auto& [slot, payload] : rejectedPayloads)
        {
            EXPECT_FALSE(author->session.CanRestoreState(payload));
            EXPECT_TRUE(StartsWithMOD350(author->engine.SaveGame(slot, payload), "Saved RPG world"));
        }
    }

    // A live, different session in a two-entity world.
    World liveWorld;
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("mod350-live-a"));
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("mod350-live-b"));
    auto live = std::make_unique<RPGQuestSlice>(&saves.System(), &liveWorld);
    ASSERT_TRUE(live->initialized);
    live->session.Travel(kForestArea); // mid-encounter, a state the saved slots do not describe
    live->npcs.AdjustDisposition(kGuardNpc, 20);
    const std::string liveState = live->session.SerializeState();
    ASSERT_TRUE(liveState != validSnapshot);

    auto expectUnchanged = [&](const std::string& result)
    {
        EXPECT_FALSE(StartsWithMOD350(result, "Loaded RPG world"));
        EXPECT_EQ(live->session.SerializeState(), liveState);
        EXPECT_EQ(liveWorld.GetEntityCount(), 2u);
    };

    for (const auto& [slot, payload] : rejectedPayloads)
    {
        const std::string result = live->LoadSlot(slot);
        EXPECT_TRUE(StartsWithMOD350(result, "Failed to validate or load RPG state"));
        expectUnchanged(result);
    }

    // Damaged engine slot files: a flipped body byte and a truncated file.
    const std::filesystem::path validPath = saves.SlotPath("valid");
    std::string validBytes;
    {
        std::ifstream input(validPath, std::ios::binary);
        validBytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    ASSERT_TRUE(validBytes.size() > 64);
    auto writeSlot = [&](const std::string& slot, const std::string& bytes)
    {
        std::ofstream output(saves.SlotPath(slot), std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return output.good();
    };
    std::string flipped = validBytes;
    const size_t payload = flipped.find("RPGDEMO 3 ");
    ASSERT_TRUE(payload != std::string::npos);
    flipped[payload + 10] = flipped[payload + 10] == '1' ? '2' : '1';
    ASSERT_TRUE(writeSlot("flipped", flipped));
    expectUnchanged(live->LoadSlot("flipped"));
    ASSERT_TRUE(writeSlot("cut", validBytes.substr(0, validBytes.size() / 2)));
    expectUnchanged(live->LoadSlot("cut"));

    // Missing and unsafe slot names.
    EXPECT_TRUE(StartsWithMOD350(live->LoadSlot("absent"), "No save found"));
    expectUnchanged(live->LoadSlot("../valid"));

    // The intact slot still loads into the same live stack.
    EXPECT_TRUE(StartsWithMOD350(live->LoadSlot("valid"), "Loaded RPG world"));
    EXPECT_EQ(live->session.SerializeState(), validSnapshot);
    EXPECT_EQ(liveWorld.GetEntityCount(), 1u);
}

TEST(RPGPersistence_LegacyV2SaveMigratesToDefaultNPCState)
{
    ScopedMOD350SaveDirectory saves("legacy_v2");
    ASSERT_TRUE(saves.IsInitialized());
    auto& quests = QuestSystem::GetInstance();

    // Author a v3 snapshot mid-quest, then cut it to exactly what a version-2 build wrote: the same player,
    // inventory, equipment and quest fields with no world clock or NPC section.
    std::vector<std::string> v3Tokens;
    std::string legacyPayload;
    size_t tailStart = 0;
    {
        World world;
        world.AddComponent<Transform>(world.CreateEntity("mod350-legacy"));
        auto author = std::make_unique<RPGQuestSlice>(&saves.System(), &world);
        ASSERT_TRUE(author->initialized);
        ASSERT_TRUE(author->Hunt("Shadow Wolf", 1));
        ASSERT_FALSE(author->session.IsInCombat());
        v3Tokens = SplitMOD350Snapshot(author->session.SerializeState());
        const size_t npcTail = 3 + author->npcs.GetNPCCount() * kMOD350NpcFields;
        ASSERT_TRUE(v3Tokens.size() > npcTail + 2);
        ASSERT_EQ(v3Tokens[1], std::string("3"));
        tailStart = v3Tokens.size() - npcTail;

        std::vector<std::string> legacy(v3Tokens.begin(), v3Tokens.begin() + static_cast<std::ptrdiff_t>(tailStart));
        legacy[1] = "2";
        legacyPayload = JoinMOD350Snapshot(legacy);
        EXPECT_TRUE(author->session.CanRestoreState(legacyPayload));
        EXPECT_TRUE(StartsWithMOD350(author->engine.SaveGame("legacy", legacyPayload), "Saved RPG world"));
    }

    // Restart, then move the live NPCs away from their defaults: the migration must not inherit them.
    World restartedWorld;
    auto restarted = std::make_unique<RPGQuestSlice>(&saves.System(), &restartedWorld);
    ASSERT_TRUE(restarted->initialized);
    restarted->npcs.Update(150.0f);
    restarted->npcs.AdjustDisposition(kBlacksmithNpc, -40);
    restarted->npcs.AdjustDisposition(kGuardNpc, 30);
    ASSERT_TRUE(restarted->npcs.GetNPC(kGuardNpc)->currentWaypointIndex != 0);

    EXPECT_TRUE(StartsWithMOD350(restarted->LoadSlot("legacy"), "Loaded RPG world"));
    EXPECT_EQ(restartedWorld.GetEntityCount(), 1u);
    EXPECT_TRUE(quests.GetQuestState(restarted->Player(), kWolfHuntQuest) == EngineQuestState::Active);
    EXPECT_EQ(restarted->ObjectiveCount(kWolfHuntQuest), 1u);

    // NPCs and the world clock are exactly a new world's defaults.
    const NPCData* blacksmith = restarted->npcs.GetNPC(kBlacksmithNpc);
    const NPCData* guard = restarted->npcs.GetNPC(kGuardNpc);
    EXPECT_EQ(blacksmith->dispositionValue, 60);
    EXPECT_TRUE(blacksmith->disposition == NPCDisposition::Friendly);
    EXPECT_EQ(guard->currentWaypointIndex, 0);
    EXPECT_NEAR(guard->waypointWaitTimer, 0.0f, 0.0001f);
    EXPECT_NEAR(restarted->npcs.GetWorldHour(), 8.0f, 0.0001f);
    const NPCSystemSnapshot migrated = restarted->npcs.CaptureState();
    const NPCSystemSnapshot defaults = RPGNPCSystem::CaptureDefaultState();
    EXPECT_NEAR(migrated.worldTime, 0.0f, 0.0001f);
    ASSERT_EQ(migrated.npcs.size(), defaults.npcs.size());
    for (size_t index = 0; index < defaults.npcs.size(); ++index)
    {
        EXPECT_EQ(migrated.npcs[index].npcId, defaults.npcs[index].npcId);
        EXPECT_EQ(migrated.npcs[index].dispositionValue, defaults.npcs[index].dispositionValue);
        EXPECT_TRUE(migrated.npcs[index].behavior == defaults.npcs[index].behavior);
        EXPECT_EQ(migrated.npcs[index].currentWaypointIndex, defaults.npcs[index].currentWaypointIndex);
    }

    // The player portion is carried over unchanged, and the next save is written as version 3.
    const std::vector<std::string> resaved = SplitMOD350Snapshot(restarted->session.SerializeState());
    ASSERT_EQ(resaved.size(), v3Tokens.size());
    EXPECT_EQ(resaved[1], std::string("3"));
    for (size_t index = 0; index < tailStart; ++index)
        EXPECT_EQ(resaved[index], v3Tokens[index]);

    const std::string resavedState = restarted->session.SerializeState();
    ASSERT_TRUE(StartsWithMOD350(restarted->SaveSlot("legacy"), "Saved RPG world"));
    restarted.reset();
    World secondWorld;
    auto second = std::make_unique<RPGQuestSlice>(&saves.System(), &secondWorld);
    ASSERT_TRUE(second->initialized);
    EXPECT_TRUE(StartsWithMOD350(second->LoadSlot("legacy"), "Loaded RPG world"));
    EXPECT_EQ(second->session.SerializeState(), resavedState);
}

TEST(RPGPersistence_NonFiniteNPCSnapshotRejectedWithoutChange)
{
    RPGNPCSystem npcs;
    ASSERT_TRUE(npcs.Initialize(nullptr));
    npcs.Update(90.0f); // move the clock and the guard's patrol off their defaults
    npcs.AdjustDisposition(kBlacksmithNpc, 15);
    const NPCSystemSnapshot before = npcs.CaptureState();
    ASSERT_TRUE(npcs.ValidateState(before));
    ASSERT_TRUE(before.npcs.size() >= 2);

    auto sameState = [](const NPCSystemSnapshot& left, const NPCSystemSnapshot& right)
    {
        if (left.worldTime != right.worldTime || left.worldHour != right.worldHour ||
            left.npcs.size() != right.npcs.size())
            return false;
        for (size_t index = 0; index < left.npcs.size(); ++index)
        {
            const NPCPersistentState& a = left.npcs[index];
            const NPCPersistentState& b = right.npcs[index];
            if (a.npcId != b.npcId || a.dispositionValue != b.dispositionValue || a.behavior != b.behavior ||
                a.posX != b.posX || a.posY != b.posY || a.posZ != b.posZ ||
                a.currentWaypointIndex != b.currentWaypointIndex || a.waypointWaitTimer != b.waypointWaitTimer)
                return false;
        }
        return true;
    };

    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    const std::vector<std::pair<const char*, void (*)(NPCSystemSnapshot&)>> mutations = {
        {"worldTime_nan", [](NPCSystemSnapshot& s) { s.worldTime = nan; }},
        {"worldTime_inf", [](NPCSystemSnapshot& s) { s.worldTime = inf; }},
        {"worldHour_nan", [](NPCSystemSnapshot& s) { s.worldHour = nan; }},
        {"worldHour_inf", [](NPCSystemSnapshot& s) { s.worldHour = inf; }},
        {"posX_nan", [](NPCSystemSnapshot& s) { s.npcs[1].posX = nan; }},
        {"posY_inf", [](NPCSystemSnapshot& s) { s.npcs[1].posY = inf; }},
        {"posZ_negative_inf", [](NPCSystemSnapshot& s) { s.npcs[0].posZ = -inf; }},
        {"wait_nan", [](NPCSystemSnapshot& s) { s.npcs[0].waypointWaitTimer = nan; }},
        {"wait_inf", [](NPCSystemSnapshot& s) { s.npcs[0].waypointWaitTimer = inf; }},
    };
    for (const auto& [name, mutate] : mutations)
    {
        NPCSystemSnapshot candidate = before;
        mutate(candidate);
        EXPECT_FALSE(npcs.ValidateState(candidate));
        EXPECT_FALSE(npcs.RestoreState(candidate));
        // Names the mutation in the failure output if a rejected snapshot leaked into the live NPCs.
        EXPECT_EQ(sameState(npcs.CaptureState(), before) ? std::string() : std::string(name), std::string());
    }

    EXPECT_TRUE(npcs.RestoreState(before));
    EXPECT_TRUE(sameState(npcs.CaptureState(), before));
    npcs.Shutdown();
}

#endif // SPARK_TEST_HAS_IMGUI
