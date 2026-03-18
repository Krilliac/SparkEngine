// TestConsoleRBAC.cpp - Tests for ConsoleRBAC (Role-Based Access Control for console commands)
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>

// ============================================================================
// Standalone reimplementation of ConsoleRBAC
// ============================================================================

namespace
{

    enum class SecurityLevel : uint32_t
    {
        Player = 0,
        Moderator = 1,
        Admin = 2,
        Developer = 3,
        Console = 4 // Internal/server console — highest privilege
    };

    class ConsoleRBAC
    {
      public:
        // Register a command with its required permission level
        void RegisterCommand(const std::string& command, SecurityLevel requiredLevel)
        {
            m_commandPermissions[command] = requiredLevel;
        }

        // Check whether a given security level has permission to execute a command
        bool HasPermission(SecurityLevel userLevel, SecurityLevel requiredLevel) const
        {
            return static_cast<uint32_t>(userLevel) >= static_cast<uint32_t>(requiredLevel);
        }

        // Check whether a specific command is allowed for a given security level
        bool IsCommandAllowed(const std::string& command, SecurityLevel userLevel) const
        {
            auto it = m_commandPermissions.find(command);
            if (it == m_commandPermissions.end())
            {
                // Unknown commands default to the lowest level (Player)
                return true;
            }
            return HasPermission(userLevel, it->second);
        }

        // Set a player's security level by ID
        void SetPlayerSecurityLevel(uint64_t playerId, SecurityLevel level) { m_playerLevels[playerId] = level; }

        // Get a player's security level (defaults to Player if not set)
        SecurityLevel GetPlayerSecurityLevel(uint64_t playerId) const
        {
            auto it = m_playerLevels.find(playerId);
            if (it != m_playerLevels.end())
            {
                return it->second;
            }
            return SecurityLevel::Player;
        }

        // Get the required security level for a command (defaults to Player if unregistered)
        SecurityLevel GetCommandSecurityLevel(const std::string& command) const
        {
            auto it = m_commandPermissions.find(command);
            if (it != m_commandPermissions.end())
            {
                return it->second;
            }
            return SecurityLevel::Player;
        }

        // Check whether a player (by ID) is allowed to run a command
        bool IsPlayerAllowed(uint64_t playerId, const std::string& command) const
        {
            return IsCommandAllowed(command, GetPlayerSecurityLevel(playerId));
        }

        size_t GetRegisteredCommandCount() const { return m_commandPermissions.size(); }

      private:
        std::unordered_map<std::string, SecurityLevel> m_commandPermissions;
        std::unordered_map<uint64_t, SecurityLevel> m_playerLevels;
    };

} // namespace

// ============================================================================
// SecurityLevel ordering
// ============================================================================

TEST(ConsoleRBAC_SecurityLevelOrdering)
{
    EXPECT_LT(static_cast<uint32_t>(SecurityLevel::Player), static_cast<uint32_t>(SecurityLevel::Moderator));
    EXPECT_LT(static_cast<uint32_t>(SecurityLevel::Moderator), static_cast<uint32_t>(SecurityLevel::Admin));
    EXPECT_LT(static_cast<uint32_t>(SecurityLevel::Admin), static_cast<uint32_t>(SecurityLevel::Developer));
    EXPECT_LT(static_cast<uint32_t>(SecurityLevel::Developer), static_cast<uint32_t>(SecurityLevel::Console));
}

// ============================================================================
// HasPermission — level comparison
// ============================================================================

TEST(ConsoleRBAC_HasPermission_SameLevel)
{
    ConsoleRBAC rbac;

    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Player, SecurityLevel::Player));
    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Admin, SecurityLevel::Admin));
    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Console, SecurityLevel::Console));
}

TEST(ConsoleRBAC_HasPermission_HigherLevelGrantsAccess)
{
    ConsoleRBAC rbac;

    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Admin, SecurityLevel::Player));
    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Admin, SecurityLevel::Moderator));
    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Console, SecurityLevel::Developer));
    EXPECT_TRUE(rbac.HasPermission(SecurityLevel::Developer, SecurityLevel::Player));
}

TEST(ConsoleRBAC_HasPermission_LowerLevelDenied)
{
    ConsoleRBAC rbac;

    EXPECT_FALSE(rbac.HasPermission(SecurityLevel::Player, SecurityLevel::Moderator));
    EXPECT_FALSE(rbac.HasPermission(SecurityLevel::Player, SecurityLevel::Admin));
    EXPECT_FALSE(rbac.HasPermission(SecurityLevel::Moderator, SecurityLevel::Admin));
    EXPECT_FALSE(rbac.HasPermission(SecurityLevel::Admin, SecurityLevel::Developer));
    EXPECT_FALSE(rbac.HasPermission(SecurityLevel::Developer, SecurityLevel::Console));
}

// ============================================================================
// SetPlayerSecurityLevel / GetPlayerSecurityLevel
// ============================================================================

TEST(ConsoleRBAC_DefaultPlayerLevel)
{
    ConsoleRBAC rbac;

    // Unregistered player defaults to Player level
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(42)), static_cast<uint32_t>(SecurityLevel::Player));
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(0)), static_cast<uint32_t>(SecurityLevel::Player));
}

TEST(ConsoleRBAC_SetAndGetPlayerLevel)
{
    ConsoleRBAC rbac;

    rbac.SetPlayerSecurityLevel(1, SecurityLevel::Moderator);
    rbac.SetPlayerSecurityLevel(2, SecurityLevel::Admin);
    rbac.SetPlayerSecurityLevel(3, SecurityLevel::Developer);

    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(1)), static_cast<uint32_t>(SecurityLevel::Moderator));
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(2)), static_cast<uint32_t>(SecurityLevel::Admin));
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(3)), static_cast<uint32_t>(SecurityLevel::Developer));
}

TEST(ConsoleRBAC_OverridePlayerLevel)
{
    ConsoleRBAC rbac;

    rbac.SetPlayerSecurityLevel(1, SecurityLevel::Player);
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(1)), static_cast<uint32_t>(SecurityLevel::Player));

    rbac.SetPlayerSecurityLevel(1, SecurityLevel::Admin);
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetPlayerSecurityLevel(1)), static_cast<uint32_t>(SecurityLevel::Admin));
}

// ============================================================================
// IsCommandAllowed — command/permission combinations
// ============================================================================

TEST(ConsoleRBAC_IsCommandAllowed_PlayerCommand)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("help", SecurityLevel::Player);

    EXPECT_TRUE(rbac.IsCommandAllowed("help", SecurityLevel::Player));
    EXPECT_TRUE(rbac.IsCommandAllowed("help", SecurityLevel::Moderator));
    EXPECT_TRUE(rbac.IsCommandAllowed("help", SecurityLevel::Admin));
    EXPECT_TRUE(rbac.IsCommandAllowed("help", SecurityLevel::Console));
}

TEST(ConsoleRBAC_IsCommandAllowed_ModeratorCommand)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("kick", SecurityLevel::Moderator);

    EXPECT_FALSE(rbac.IsCommandAllowed("kick", SecurityLevel::Player));
    EXPECT_TRUE(rbac.IsCommandAllowed("kick", SecurityLevel::Moderator));
    EXPECT_TRUE(rbac.IsCommandAllowed("kick", SecurityLevel::Admin));
}

TEST(ConsoleRBAC_IsCommandAllowed_AdminCommand)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("ban", SecurityLevel::Admin);

    EXPECT_FALSE(rbac.IsCommandAllowed("ban", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("ban", SecurityLevel::Moderator));
    EXPECT_TRUE(rbac.IsCommandAllowed("ban", SecurityLevel::Admin));
    EXPECT_TRUE(rbac.IsCommandAllowed("ban", SecurityLevel::Developer));
    EXPECT_TRUE(rbac.IsCommandAllowed("ban", SecurityLevel::Console));
}

TEST(ConsoleRBAC_IsCommandAllowed_DeveloperCommand)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("reload_scripts", SecurityLevel::Developer);

    EXPECT_FALSE(rbac.IsCommandAllowed("reload_scripts", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("reload_scripts", SecurityLevel::Admin));
    EXPECT_TRUE(rbac.IsCommandAllowed("reload_scripts", SecurityLevel::Developer));
    EXPECT_TRUE(rbac.IsCommandAllowed("reload_scripts", SecurityLevel::Console));
}

TEST(ConsoleRBAC_IsCommandAllowed_ConsoleOnly)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("shutdown", SecurityLevel::Console);

    EXPECT_FALSE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Moderator));
    EXPECT_FALSE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Admin));
    EXPECT_FALSE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Developer));
    EXPECT_TRUE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Console));
}

// ============================================================================
// Edge cases — unknown commands default to lowest level
// ============================================================================

TEST(ConsoleRBAC_UnknownCommandDefaultsToPlayer)
{
    ConsoleRBAC rbac;

    // No commands registered — unknown command should be allowed by anyone
    EXPECT_TRUE(rbac.IsCommandAllowed("nonexistent_command", SecurityLevel::Player));
    EXPECT_TRUE(rbac.IsCommandAllowed("anything", SecurityLevel::Moderator));
}

TEST(ConsoleRBAC_GetCommandSecurityLevel_Unknown)
{
    ConsoleRBAC rbac;

    EXPECT_EQ(static_cast<uint32_t>(rbac.GetCommandSecurityLevel("unknown")),
              static_cast<uint32_t>(SecurityLevel::Player));
}

TEST(ConsoleRBAC_GetCommandSecurityLevel_Registered)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("ban", SecurityLevel::Admin);

    EXPECT_EQ(static_cast<uint32_t>(rbac.GetCommandSecurityLevel("ban")), static_cast<uint32_t>(SecurityLevel::Admin));
}

// ============================================================================
// IsPlayerAllowed — combines player level lookup with command check
// ============================================================================

TEST(ConsoleRBAC_IsPlayerAllowed)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("help", SecurityLevel::Player);
    rbac.RegisterCommand("kick", SecurityLevel::Moderator);
    rbac.RegisterCommand("ban", SecurityLevel::Admin);

    rbac.SetPlayerSecurityLevel(1, SecurityLevel::Player);
    rbac.SetPlayerSecurityLevel(2, SecurityLevel::Moderator);
    rbac.SetPlayerSecurityLevel(3, SecurityLevel::Admin);

    // Player can use help, not kick or ban
    EXPECT_TRUE(rbac.IsPlayerAllowed(1, "help"));
    EXPECT_FALSE(rbac.IsPlayerAllowed(1, "kick"));
    EXPECT_FALSE(rbac.IsPlayerAllowed(1, "ban"));

    // Moderator can use help and kick, not ban
    EXPECT_TRUE(rbac.IsPlayerAllowed(2, "help"));
    EXPECT_TRUE(rbac.IsPlayerAllowed(2, "kick"));
    EXPECT_FALSE(rbac.IsPlayerAllowed(2, "ban"));

    // Admin can use all three
    EXPECT_TRUE(rbac.IsPlayerAllowed(3, "help"));
    EXPECT_TRUE(rbac.IsPlayerAllowed(3, "kick"));
    EXPECT_TRUE(rbac.IsPlayerAllowed(3, "ban"));
}

TEST(ConsoleRBAC_IsPlayerAllowed_UnknownPlayer)
{
    ConsoleRBAC rbac;
    rbac.RegisterCommand("kick", SecurityLevel::Moderator);

    // Unknown player defaults to Player level — cannot use Moderator command
    EXPECT_FALSE(rbac.IsPlayerAllowed(999, "kick"));
}

TEST(ConsoleRBAC_IsPlayerAllowed_UnknownCommand)
{
    ConsoleRBAC rbac;
    rbac.SetPlayerSecurityLevel(1, SecurityLevel::Player);

    // Unknown command defaults to Player level — any player can run it
    EXPECT_TRUE(rbac.IsPlayerAllowed(1, "unknown_cmd"));
}

// ============================================================================
// RegisterCommand and GetRegisteredCommandCount
// ============================================================================

TEST(ConsoleRBAC_RegisterCommandCount)
{
    ConsoleRBAC rbac;

    EXPECT_EQ(rbac.GetRegisteredCommandCount(), 0u);

    rbac.RegisterCommand("help", SecurityLevel::Player);
    EXPECT_EQ(rbac.GetRegisteredCommandCount(), 1u);

    rbac.RegisterCommand("kick", SecurityLevel::Moderator);
    rbac.RegisterCommand("ban", SecurityLevel::Admin);
    EXPECT_EQ(rbac.GetRegisteredCommandCount(), 3u);
}

TEST(ConsoleRBAC_RegisterCommandOverwrite)
{
    ConsoleRBAC rbac;

    // Registering the same command again overwrites the permission level
    rbac.RegisterCommand("teleport", SecurityLevel::Admin);
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetCommandSecurityLevel("teleport")),
              static_cast<uint32_t>(SecurityLevel::Admin));

    rbac.RegisterCommand("teleport", SecurityLevel::Moderator);
    EXPECT_EQ(static_cast<uint32_t>(rbac.GetCommandSecurityLevel("teleport")),
              static_cast<uint32_t>(SecurityLevel::Moderator));

    // Count should still be 1 (overwrite, not duplicate)
    EXPECT_EQ(rbac.GetRegisteredCommandCount(), 1u);
}

// ============================================================================
// Multiple commands across all levels
// ============================================================================

TEST(ConsoleRBAC_FullHierarchy)
{
    ConsoleRBAC rbac;

    rbac.RegisterCommand("say", SecurityLevel::Player);
    rbac.RegisterCommand("mute", SecurityLevel::Moderator);
    rbac.RegisterCommand("ban", SecurityLevel::Admin);
    rbac.RegisterCommand("reload_scripts", SecurityLevel::Developer);
    rbac.RegisterCommand("shutdown", SecurityLevel::Console);

    // Player: only say
    EXPECT_TRUE(rbac.IsCommandAllowed("say", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("mute", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("ban", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("reload_scripts", SecurityLevel::Player));
    EXPECT_FALSE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Player));

    // Console: everything
    EXPECT_TRUE(rbac.IsCommandAllowed("say", SecurityLevel::Console));
    EXPECT_TRUE(rbac.IsCommandAllowed("mute", SecurityLevel::Console));
    EXPECT_TRUE(rbac.IsCommandAllowed("ban", SecurityLevel::Console));
    EXPECT_TRUE(rbac.IsCommandAllowed("reload_scripts", SecurityLevel::Console));
    EXPECT_TRUE(rbac.IsCommandAllowed("shutdown", SecurityLevel::Console));
}
