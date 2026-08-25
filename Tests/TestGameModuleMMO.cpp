/**
 * @file TestGameModuleMMO.cpp
 * @brief Tests for MMO game module character, social, world, and gameplay systems
 *
 * All tests are behind SPARK_TEST_HAS_IMGUI because the MMO module .cpp
 * files include ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.h"
#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "../GameModules/SparkGameMMO/Source/Crafting/MMOCraftingSystem.h"
#include "../GameModules/SparkGameMMO/Source/Guild/MMOGuildSystem.h"
#include "../GameModules/SparkGameMMO/Source/Inventory/MMOInventorySystem.h"
#include "../GameModules/SparkGameMMO/Source/Player/MMOPlayerSystem.h"
#include "../GameModules/SparkGameMMO/Source/Trading/MMOTradingSystem.h"
#include "../GameModules/SparkGameMMO/Source/UI/MMOLoginUI.h"
#include "../GameModules/SparkGameMMO/Source/World/MMOWorldSetup.h"
#include "../GameModules/SparkGameMMO/Source/WorldBoss/MMOWorldBossSystem.h"

#include "../GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.h"

#include <cmath>
#include <limits>

using namespace MMO;

namespace
{
    class MMOTestContext final : public Spark::IEngineContext
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
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
    };
} // namespace

// ============================================================================
// MMOCharacterSystem
// ============================================================================

TEST(MMO_CharacterSystem_Initialize)
{
    MMOCharacterSystem sys;
    EXPECT_TRUE(sys.Initialize(nullptr));
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_GetAllRaces_NonEmpty)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);
    EXPECT_FALSE(sys.GetAllRaces().empty());
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_GetAllClasses_NonEmpty)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);
    EXPECT_FALSE(sys.GetAllClasses().empty());
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_IsValidCombination)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);

    // At least one race/class combo must be valid
    bool foundValid = false;
    for (const auto& race : sys.GetAllRaces())
    {
        for (const auto& cls : sys.GetAllClasses())
        {
            if (sys.IsValidCombination(race.id, cls.id))
            {
                foundValid = true;
                break;
            }
        }
        if (foundValid)
            break;
    }
    EXPECT_TRUE(foundValid);
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_CreateCharacter)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);

    CharacterCreateRequest req;
    req.name = "Aldric";
    req.race = RaceId::Human;
    req.classId = ClassId::Warrior;

    auto result = sys.CreateCharacter(1, req);
    EXPECT_TRUE(result.success);
    EXPECT_GT(result.characterId, 0u);
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_GetCharacters)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);

    CharacterCreateRequest req;
    req.name = "Elara";
    req.race = RaceId::Elf;
    req.classId = ClassId::Mage;
    sys.CreateCharacter(1, req);

    auto chars = sys.GetCharacters(1);
    EXPECT_FALSE(chars.empty());
    EXPECT_EQ(chars[0].name, std::string("Elara"));
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_IsNameAvailable)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);

    EXPECT_TRUE(sys.IsNameAvailable("Thorin"));

    CharacterCreateRequest req;
    req.name = "Thorin";
    req.race = RaceId::Dwarf;
    req.classId = ClassId::Warrior;
    sys.CreateCharacter(1, req);

    EXPECT_FALSE(sys.IsNameAvailable("Thorin"));
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_GetTotalCharacters)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);

    size_t before = sys.GetTotalCharacters();

    CharacterCreateRequest req;
    req.name = "Kira";
    req.race = RaceId::Human;
    req.classId = ClassId::Ranger;
    sys.CreateCharacter(1, req);

    EXPECT_EQ(sys.GetTotalCharacters(), before + 1);
    sys.Shutdown();
}

TEST(MMO_CharacterSystem_ComputeStats)
{
    MMOCharacterSystem sys;
    sys.Initialize(nullptr);

    auto stats = sys.ComputeStats(RaceId::Orc, ClassId::Warrior, 1);
    EXPECT_GT(stats.health, 0.0f);
    EXPECT_GT(stats.strength, 0.0f);
    sys.Shutdown();
}

// ============================================================================
// MMOGuildSystem
// ============================================================================

TEST(MMO_GuildSystem_Initialize)
{
    MMOGuildSystem sys;
    EXPECT_TRUE(sys.Initialize(nullptr));
    sys.Shutdown();
}

TEST(MMO_GuildSystem_CreateGuild)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Iron Wolves", "IW", 100, "Aldric");
    EXPECT_GT(guildId, 0u);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_GetGuild)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Silver Dawn", "SD", 200, "Elara");
    const Guild* guild = sys.GetGuild(guildId);
    EXPECT_TRUE(guild != nullptr);
    EXPECT_EQ(guild->name, std::string("Silver Dawn"));
    EXPECT_EQ(guild->leaderId, 200u);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_InviteMember)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Storm Guard", "SG", 100, "Aldric");
    bool invited = sys.InviteMember(guildId, 100, 101, "Kira");
    EXPECT_TRUE(invited);

    const Guild* guild = sys.GetGuild(guildId);
    EXPECT_GE(guild->GetMemberCount(), 2);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_PromoteDemoteMember)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Ashen Order", "AO", 100, "Aldric");
    sys.InviteMember(guildId, 100, 101, "Kira");

    bool promoted = sys.PromoteMember(guildId, 100, 101);
    EXPECT_TRUE(promoted);

    bool demoted = sys.DemoteMember(guildId, 100, 101);
    EXPECT_TRUE(demoted);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_SetMotd)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Crimson Blade", "CB", 100, "Aldric");
    bool set = sys.SetMotd(guildId, 100, "Welcome to the guild!");
    EXPECT_TRUE(set);

    const Guild* guild = sys.GetGuild(guildId);
    EXPECT_EQ(guild->motd, std::string("Welcome to the guild!"));
    sys.Shutdown();
}

TEST(MMO_GuildSystem_DepositWithdrawCurrency)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Golden Hoard", "GH", 100, "Aldric");
    EXPECT_TRUE(sys.DepositCurrency(guildId, 500));

    const Guild* guild = sys.GetGuild(guildId);
    EXPECT_EQ(guild->bankCurrency, 500);

    EXPECT_TRUE(sys.WithdrawCurrency(guildId, 100, 200));
    guild = sys.GetGuild(guildId);
    EXPECT_EQ(guild->bankCurrency, 300);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_LeaveGuild)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Night Watch", "NW", 100, "Aldric");
    sys.InviteMember(guildId, 100, 101, "Kira");

    int membersBefore = sys.GetGuild(guildId)->GetMemberCount();
    EXPECT_TRUE(sys.LeaveGuild(guildId, 101));
    EXPECT_LT(sys.GetGuild(guildId)->GetMemberCount(), membersBefore);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_GetGuildCount)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    size_t before = sys.GetGuildCount();
    sys.CreateGuild("Guild A", "GA", 100, "Player1");
    sys.CreateGuild("Guild B", "GB", 200, "Player2");
    EXPECT_EQ(sys.GetGuildCount(), before + 2);
    sys.Shutdown();
}

TEST(MMO_GuildSystem_DisbandGuild)
{
    MMOGuildSystem sys;
    sys.Initialize(nullptr);

    uint32_t guildId = sys.CreateGuild("Temp Guild", "TG", 100, "Aldric");
    EXPECT_TRUE(sys.DisbandGuild(guildId, 100));
    EXPECT_TRUE(sys.GetGuild(guildId) == nullptr);
    sys.Shutdown();
}

// ============================================================================
// MMOChatSystem
// ============================================================================

TEST(MMO_ChatSystem_Initialize)
{
    MMOChatSystem sys;
    EXPECT_TRUE(sys.Initialize(nullptr));
    sys.Shutdown();
}

TEST(MMO_ChatSystem_SendMessage)
{
    MMOChatSystem sys;
    sys.Initialize(nullptr);

    EXPECT_NO_THROW(sys.SendMessage(ChatChannel::Global, "Hello world!"));
    sys.Shutdown();
}

TEST(MMO_ChatSystem_GetHistory)
{
    MMOChatSystem sys;
    sys.Initialize(nullptr);

    sys.SendMessage(ChatChannel::Global, "Test message");
    const auto& history = sys.GetHistory();
    EXPECT_FALSE(history.empty());
    EXPECT_EQ(history.back().text, std::string("Test message"));
    sys.Shutdown();
}

TEST(MMO_ChatSystem_GetChannelCount)
{
    MMOChatSystem sys;
    sys.Initialize(nullptr);

    EXPECT_GT(sys.GetChannelCount(), 0u);
    sys.Shutdown();
}

TEST(MMO_ChatSystem_RejectsEmptyMessage)
{
    MMOChatSystem sys;
    sys.Initialize(nullptr);

    const size_t historyBefore = sys.GetHistory().size();
    sys.SendMessage(ChatChannel::Global, "");
    EXPECT_EQ(sys.GetHistory().size(), historyBefore);
    sys.Shutdown();
}

// ============================================================================
// Player movement and area resolution
// ============================================================================

TEST(MMO_PlayerMovement_NormalizesDiagonalInput)
{
    MMOPlayer player;
    MMOPlayerSystem::IntegrateMovement(player, MMOPlayerInput{1.0f, 1.0f, false}, 0.1f);

    const float velocity = std::sqrt(player.velocityX * player.velocityX + player.velocityZ * player.velocityZ);
    const float distance = std::sqrt(player.posX * player.posX + player.posZ * player.posZ);
    EXPECT_NEAR(velocity, 6.0f, 0.0001f);
    EXPECT_NEAR(distance, 0.6f, 0.0001f);
    EXPECT_NEAR(player.posX, player.posZ, 0.0001f);
}

TEST(MMO_PlayerMovement_ClampsLongFrames)
{
    MMOPlayer player;
    MMOPlayerSystem::IntegrateMovement(player, MMOPlayerInput{1.0f, 0.0f, true}, 2.0f);

    EXPECT_NEAR(player.velocityX, 10.5f, 0.0001f);
    EXPECT_NEAR(player.posX, 2.625f, 0.0001f);
    EXPECT_NEAR(player.posZ, 0.0f, 0.0001f);
}

TEST(MMO_PlayerMovement_RejectsNonFiniteInput)
{
    MMOPlayer player;
    MMOPlayerSystem::IntegrateMovement(player, MMOPlayerInput{std::numeric_limits<float>::quiet_NaN(), 1.0f, false},
                                       0.1f);

    EXPECT_TRUE(std::isfinite(player.posX));
    EXPECT_TRUE(std::isfinite(player.posZ));
    EXPECT_NEAR(player.posX, 0.0f, 0.0001f);
    EXPECT_NEAR(player.posZ, 0.6f, 0.0001f);
}

TEST(MMO_WorldAreaResolution_PrefersCurrentOverlappingArea)
{
    MMOTestContext context;
    MMOWorldSetup world;
    EXPECT_TRUE(world.Initialize(&context));

    // TownSquare and ShadowCrypt overlap at this point. An explicitly entered
    // dungeon must remain selected instead of snapping back to the outdoor area.
    EXPECT_EQ(world.FindAreaId(0.0f, 0.0f, 0.0f, 3), 3u);
    EXPECT_EQ(world.FindAreaId(0.0f, 0.0f, 0.0f, 1), 1u);
    EXPECT_EQ(world.FindAreaId(550.0f, 1.0f, 0.0f, 1), 2u);
    world.Shutdown();
}

// ============================================================================
// Inventory-backed crafting
// ============================================================================

TEST(MMO_Crafting_RejectsMissingInventoryDependency)
{
    MMOCraftingSystem crafting;
    crafting.Initialize(nullptr);
    CraftingState state;
    state.nearbyStation = CraftingStation::AlchemyLab;
    state.skills[static_cast<int>(CraftingDiscipline::Alchemy)] = {CraftingDiscipline::Alchemy, 1, 0, 100};
    crafting.LearnRecipe(state, 100);
    InventoryData inventory;
    inventory.slots = {{10, 2}, {11, 1}};

    EXPECT_FALSE(crafting.CanCraft(state, inventory, 100));
    EXPECT_FALSE(crafting.StartCraft(state, inventory, 100));
    EXPECT_EQ(inventory.slots.size(), 2u);
    crafting.Shutdown();
}

TEST(MMO_Crafting_ConsumesMaterialsAndProducesResult)
{
    MMOInventorySystem items;
    MMOCraftingSystem crafting;
    items.Initialize(nullptr);
    crafting.Initialize(nullptr, &items);

    CraftingState state;
    state.nearbyStation = CraftingStation::AlchemyLab;
    state.skills[static_cast<int>(CraftingDiscipline::Alchemy)] = {CraftingDiscipline::Alchemy, 1, 0, 100};
    crafting.LearnRecipe(state, 100);
    InventoryData inventory;
    items.AddItem(inventory, 10, 2);
    items.AddItem(inventory, 11, 1);

    EXPECT_TRUE(crafting.StartCraft(state, inventory, 100));
    EXPECT_EQ(items.CountItem(inventory, 10), 0);
    EXPECT_EQ(items.CountItem(inventory, 11), 0);
    crafting.Update(3.1f, state, inventory);
    EXPECT_FALSE(state.isCrafting);
    EXPECT_EQ(items.CountItem(inventory, 1), 1);
    EXPECT_EQ(state.skills[static_cast<int>(CraftingDiscipline::Alchemy)].currentXP, 15);

    crafting.Shutdown();
    items.Shutdown();
}

TEST(MMO_Crafting_HoldsCompletedResultUntilSpaceIsAvailable)
{
    MMOInventorySystem items;
    MMOCraftingSystem crafting;
    items.Initialize(nullptr);
    crafting.Initialize(nullptr, &items);

    CraftingState state;
    state.nearbyStation = CraftingStation::AlchemyLab;
    state.skills[static_cast<int>(CraftingDiscipline::Alchemy)] = {CraftingDiscipline::Alchemy, 1, 0, 100};
    crafting.LearnRecipe(state, 100);
    InventoryData inventory;
    inventory.maxSlots = 2;
    items.AddItem(inventory, 10, 2);
    items.AddItem(inventory, 11, 1);
    EXPECT_TRUE(crafting.StartCraft(state, inventory, 100));

    // Fill the slots freed by ingredient consumption while the timed craft runs.
    items.AddItem(inventory, 13, 1);
    items.AddItem(inventory, 14, 1);
    crafting.Update(3.1f, state, inventory);
    EXPECT_TRUE(state.isCrafting);
    EXPECT_EQ(items.CountItem(inventory, 1), 0);

    items.RemoveItem(inventory, 14, 1);
    crafting.Update(0.1f, state, inventory);
    EXPECT_FALSE(state.isCrafting);
    EXPECT_EQ(items.CountItem(inventory, 1), 1);

    crafting.Shutdown();
    items.Shutdown();
}

TEST(MMO_Trading_TransfersOffersAtomically)
{
    MMOInventorySystem items;
    MMOTradingSystem trading;
    items.Initialize(nullptr);
    trading.Initialize(nullptr);

    InventoryData inventoryA;
    InventoryData inventoryB;
    inventoryA.currency = 50;
    inventoryB.currency = 30;
    items.AddItem(inventoryA, 10, 2);
    items.AddItem(inventoryB, 13, 1);

    const uint32_t tradeId = trading.ProposeTrade(1, 2);
    EXPECT_GT(tradeId, 0u);
    EXPECT_TRUE(trading.AcceptTrade(tradeId, 2));
    EXPECT_TRUE(trading.AddTradeItem(tradeId, 1, 10, 2));
    EXPECT_TRUE(trading.AddTradeItem(tradeId, 2, 13, 1));
    EXPECT_TRUE(trading.SetTradeCurrency(tradeId, 1, 10));
    EXPECT_TRUE(trading.SetTradeCurrency(tradeId, 2, 5));
    EXPECT_TRUE(trading.ConfirmTrade(tradeId, 1));
    EXPECT_TRUE(trading.ConfirmTrade(tradeId, 2));
    EXPECT_TRUE(trading.ExecuteTrade(tradeId, inventoryA, inventoryB, items));

    EXPECT_EQ(items.CountItem(inventoryA, 10), 0);
    EXPECT_EQ(items.CountItem(inventoryA, 13), 1);
    EXPECT_EQ(items.CountItem(inventoryB, 13), 0);
    EXPECT_EQ(items.CountItem(inventoryB, 10), 2);
    EXPECT_EQ(inventoryA.currency, 45);
    EXPECT_EQ(inventoryB.currency, 35);

    trading.Shutdown();
    items.Shutdown();
}

TEST(MMO_Trading_RollsBackWhenAnOfferIsNotOwned)
{
    MMOInventorySystem items;
    MMOTradingSystem trading;
    items.Initialize(nullptr);
    trading.Initialize(nullptr);

    InventoryData inventoryA;
    InventoryData inventoryB;
    inventoryA.currency = 25;
    inventoryB.currency = 25;
    const uint32_t tradeId = trading.ProposeTrade(1, 2);
    trading.AcceptTrade(tradeId, 2);
    trading.AddTradeItem(tradeId, 1, 10, 1);
    trading.ConfirmTrade(tradeId, 1);
    trading.ConfirmTrade(tradeId, 2);

    EXPECT_FALSE(trading.ExecuteTrade(tradeId, inventoryA, inventoryB, items));
    EXPECT_TRUE(inventoryA.slots.empty());
    EXPECT_TRUE(inventoryB.slots.empty());
    EXPECT_EQ(inventoryA.currency, 25);
    EXPECT_EQ(inventoryB.currency, 25);

    trading.Shutdown();
    items.Shutdown();
}

TEST(MMO_AuctionBuyout_DoesNotChargeWhenInventoryIsFull)
{
    MMOInventorySystem items;
    MMOTradingSystem trading;
    items.Initialize(nullptr);
    trading.Initialize(nullptr);

    const uint32_t listingId = trading.CreateListing(1, "Seller", 10, 1, 5, 10, AuctionDuration::Short);
    InventoryData buyer;
    buyer.maxSlots = 0;
    buyer.currency = 50;
    EXPECT_FALSE(trading.Buyout(listingId, 2, buyer, items));
    EXPECT_EQ(buyer.currency, 50);
    EXPECT_TRUE(buyer.slots.empty());

    trading.Shutdown();
    items.Shutdown();
}

// ============================================================================
// Runtime UI and combat validation
// ============================================================================

TEST(MMO_LoginUI_EnteringWorldProgressesWithoutRendering)
{
    MMOAccountSystem accounts;
    MMOCharacterSystem characters;
    MMOLoginUI login;
    accounts.Initialize(nullptr);
    characters.Initialize(nullptr);

    EXPECT_TRUE(login.Initialize(nullptr, &accounts, &characters));
    login.SetState(LoginUIState::EnteringWorld);
    login.Update(1.0f);
    EXPECT_EQ(static_cast<int>(login.GetState()), static_cast<int>(LoginUIState::EnteringWorld));
    login.Update(1.1f);
    EXPECT_EQ(static_cast<int>(login.GetState()), static_cast<int>(LoginUIState::InGame));

    login.Shutdown();
    characters.Shutdown();
    accounts.Shutdown();
}

TEST(MMO_WorldBoss_RejectsNonPositiveOrNonFiniteContribution)
{
    MMOWorldBossSystem bosses;
    bosses.Initialize(nullptr);
    EXPECT_TRUE(bosses.SpawnBoss(1));
    const auto* before = bosses.GetBossInstance(1);
    EXPECT_TRUE(before != nullptr);
    const float initialHealth = before->currentHealth;

    EXPECT_FALSE(bosses.DamageBoss(1, 7, "Tester", 0.0f));
    EXPECT_FALSE(bosses.DamageBoss(1, 7, "Tester", -100.0f));
    EXPECT_FALSE(bosses.DamageBoss(1, 7, "Tester", std::numeric_limits<float>::quiet_NaN()));
    const auto* after = bosses.GetBossInstance(1);
    EXPECT_NEAR(after->currentHealth, initialHealth, 0.0001f);
    EXPECT_TRUE(after->contributions.empty());
    bosses.Shutdown();
}

#endif // SPARK_TEST_HAS_IMGUI
