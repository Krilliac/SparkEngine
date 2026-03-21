/**
 * @file MMOAccountSystem.h
 * @brief Account management: registration, login, sessions, bans
 * @author Spark Engine Team
 * @date 2026
 *
 * Server-authoritative account system with:
 * - Username/password registration with salted hashing
 * - Login authentication returning session tokens
 * - Session validation and timeout
 * - Account status (active, suspended, banned)
 * - Character slot limits per account
 * - Login history tracking
 */

#pragma once

#include "Spark/IEngineContext.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMO
{

    /// @brief Account status flags
    enum class AccountStatus : uint8_t
    {
        Active,
        Suspended,
        Banned,
        PendingVerification,
        Locked
    };

    /// @brief Account tier for subscription or progression gating
    enum class AccountTier : uint8_t
    {
        Free,
        Standard,
        Premium,
        Admin
    };

    /// @brief A single login history entry
    struct LoginRecord
    {
        uint64_t timestamp = 0;
        std::string ipAddress;
        bool success = false;
    };

    /// @brief Account data stored in the database
    struct AccountData
    {
        uint32_t accountId = 0;
        std::string username;
        std::string passwordHash;
        std::string salt;
        std::string email;
        AccountStatus status = AccountStatus::Active;
        AccountTier tier = AccountTier::Free;
        int maxCharacterSlots = 4;
        uint64_t createdAt = 0;
        uint64_t lastLogin = 0;
        uint64_t banExpiry = 0;
        std::string banReason;
        int failedLoginAttempts = 0;
        float totalPlayTimeHours = 0.0f;
        std::vector<LoginRecord> loginHistory;
    };

    /// @brief Active session for a logged-in account
    struct SessionData
    {
        uint64_t sessionToken = 0;
        uint32_t accountId = 0;
        std::string username;
        AccountTier tier = AccountTier::Free;
        uint64_t loginTime = 0;
        uint64_t lastActivity = 0;
        float sessionDuration = 0.0f;
        uint32_t activeCharacterId = 0;
        bool authenticated = false;
    };

    /// @brief Result of a login or registration attempt
    struct AuthResult
    {
        bool success = false;
        std::string errorMessage;
        uint32_t accountId = 0;
        uint64_t sessionToken = 0;
    };

    /**
     * @brief Account registration, authentication, and session management
     */
    class MMOAccountSystem
    {
      public:
        MMOAccountSystem() = default;
        ~MMOAccountSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float dt);
        void Shutdown();
        void RenderDebugUI();

        // === Registration ===
        AuthResult Register(const std::string& username, const std::string& password, const std::string& email = "");

        // === Authentication ===
        AuthResult Login(const std::string& username, const std::string& password);
        void Logout(uint64_t sessionToken);
        bool ValidateSession(uint64_t sessionToken) const;

        // === Session ===
        const SessionData* GetSession(uint64_t sessionToken) const;
        SessionData* GetSessionMut(uint64_t sessionToken);
        const SessionData* GetSessionByAccount(uint32_t accountId) const;
        bool SetActiveCharacter(uint64_t sessionToken, uint32_t characterId);

        // === Account Management ===
        const AccountData* GetAccount(uint32_t accountId) const;
        const AccountData* FindAccount(const std::string& username) const;
        bool ChangePassword(uint32_t accountId, const std::string& oldPass, const std::string& newPass);
        bool SetAccountStatus(uint32_t accountId, AccountStatus status, const std::string& reason = "");
        bool BanAccount(uint32_t accountId, float durationHours, const std::string& reason);
        bool UnbanAccount(uint32_t accountId);
        int GetCharacterSlots(uint32_t accountId) const;

        // === Queries ===
        bool IsLoggedIn(uint32_t accountId) const;
        size_t GetOnlineCount() const { return m_sessions.size(); }
        size_t GetAccountCount() const { return m_accounts.size(); }

        std::string GetAccountInfoString(uint32_t accountId) const;
        std::string GetOnlineListString() const;

      private:
        static std::string GenerateSalt();
        static std::string HashPassword(const std::string& password, const std::string& salt);
        static uint64_t GenerateSessionToken();
        static uint64_t GetTimestamp();
        void CleanExpiredSessions();
        void CleanExpiredBans();

        Spark::IEngineContext* m_context{nullptr};
        std::unordered_map<uint32_t, AccountData> m_accounts;
        std::unordered_map<uint64_t, SessionData> m_sessions;
        uint32_t m_nextAccountId = 1;

        static constexpr float SESSION_TIMEOUT = 1800.0f; // 30 minutes idle
        static constexpr int MAX_FAILED_LOGINS = 5;
        static constexpr float LOCKOUT_DURATION = 900.0f; // 15 min lockout
        static constexpr int MAX_LOGIN_HISTORY = 20;
        static constexpr float SESSION_CLEANUP_INTERVAL = 60.0f;

        float m_cleanupTimer = SESSION_CLEANUP_INTERVAL;
    };

} // namespace MMO
