/**
 * @file TestGameModuleMMO.cpp
 * @brief Tests for MMO game module systems: character, guild, and chat
 *
 * All tests are behind SPARK_TEST_HAS_IMGUI because the MMO module .cpp
 * files include ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameMMO/Source/Character/MMOCharacterSystem.h"
#include "../GameModules/SparkGameMMO/Source/Guild/MMOGuildSystem.h"
#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"

using namespace MMO;

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

#endif // SPARK_TEST_HAS_IMGUI
