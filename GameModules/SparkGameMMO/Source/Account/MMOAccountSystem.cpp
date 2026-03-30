/**
 * @file MMOAccountSystem.cpp
 * @brief Account registration, login authentication, and session management
 */

#include "MMOAccountSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <chrono>
#include <functional>
#include <random>
#include <sstream>
#include "Utils/LogMacros.h"

namespace MMO
{

    bool MMOAccountSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Account system initialized");
        return true;
    }

    void MMOAccountSystem::Update(float dt)
    {
        m_cleanupTimer -= dt;
        if (m_cleanupTimer <= 0.0f)
        {
            CleanExpiredSessions();
            CleanExpiredBans();
            m_cleanupTimer = SESSION_CLEANUP_INTERVAL;
        }

        // Update session durations
        for (auto& [token, session] : m_sessions)
            session.sessionDuration += dt;
    }

    void MMOAccountSystem::Shutdown()
    {
        // Log out all active sessions
        for (auto& [token, session] : m_sessions)
        {
            auto it = m_accounts.find(session.accountId);
            if (it != m_accounts.end())
                it->second.totalPlayTimeHours += session.sessionDuration / 3600.0f;
        }
        m_sessions.clear();
        m_accounts.clear();
    }

    // === Hashing utilities ===

    std::string MMOAccountSystem::GenerateSalt()
    {
        static std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<uint64_t> dist;
        uint64_t val = dist(rng);
        std::ostringstream ss;
        ss << std::hex << val;
        return ss.str();
    }

    std::string MMOAccountSystem::HashPassword(const std::string& password, const std::string& salt)
    {
        // Simple hash for demonstration — in production use bcrypt/scrypt/argon2
        std::string combined = salt + password + salt;
        size_t hash = std::hash<std::string>{}(combined);
        // Double-hash for basic strengthening
        hash ^= std::hash<std::string>{}(std::to_string(hash) + combined);
        std::ostringstream ss;
        ss << std::hex << hash;
        return ss.str();
    }

    uint64_t MMOAccountSystem::GenerateSessionToken()
    {
        static std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<uint64_t> dist;
        return dist(rng);
    }

    uint64_t MMOAccountSystem::GetTimestamp()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // === Registration ===

    AuthResult MMOAccountSystem::Register(const std::string& username, const std::string& password,
                                          const std::string& email)
    {
        AuthResult result;

        // Validate input
        if (username.size() < 3 || username.size() > 24)
        {
            result.errorMessage = "Username must be 3-24 characters";
            return result;
        }
        if (password.size() < 6)
        {
            result.errorMessage = "Password must be at least 6 characters";
            return result;
        }

        // Check uniqueness
        if (FindAccount(username))
        {
            result.errorMessage = "Username already taken";
            return result;
        }

        // Create account
        AccountData account;
        account.accountId = m_nextAccountId++;
        account.username = username;
        account.salt = GenerateSalt();
        account.passwordHash = HashPassword(password, account.salt);
        account.email = email;
        account.status = AccountStatus::Active;
        account.tier = AccountTier::Free;
        account.createdAt = GetTimestamp();

        uint32_t id = account.accountId;
        m_accounts[id] = std::move(account);

        result.success = true;
        result.accountId = id;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Account registered: %s (ID %s)", username.c_str(),
                       std::to_string(id).c_str());
        return result;
    }

    // === Authentication ===

    AuthResult MMOAccountSystem::Login(const std::string& username, const std::string& password)
    {
        AuthResult result;
        uint64_t now = GetTimestamp();

        // Find account
        AccountData* account = nullptr;
        for (auto& [id, acct] : m_accounts)
        {
            if (acct.username == username)
            {
                account = &acct;
                break;
            }
        }

        if (!account)
        {
            result.errorMessage = "Invalid username or password";
            return result;
        }

        // Check account status
        if (account->status == AccountStatus::Banned)
        {
            if (account->banExpiry == 0 || account->banExpiry > now)
            {
                result.errorMessage = "Account banned: " + account->banReason;
                return result;
            }
            // Ban expired
            account->status = AccountStatus::Active;
            account->banExpiry = 0;
            account->banReason.clear();
        }

        if (account->status == AccountStatus::Suspended)
        {
            result.errorMessage = "Account suspended";
            return result;
        }

        if (account->status == AccountStatus::Locked)
        {
            result.errorMessage = "Account locked due to too many failed login attempts";
            return result;
        }

        // Verify password
        std::string hash = HashPassword(password, account->salt);
        if (hash != account->passwordHash)
        {
            account->failedLoginAttempts++;
            if (account->failedLoginAttempts >= MAX_FAILED_LOGINS)
                account->status = AccountStatus::Locked;

            // Log failed attempt
            LoginRecord record;
            record.timestamp = now;
            record.success = false;
            account->loginHistory.push_back(record);
            if (account->loginHistory.size() > MAX_LOGIN_HISTORY)
                account->loginHistory.erase(account->loginHistory.begin());

            result.errorMessage = "Invalid username or password";
            return result;
        }

        // Check if already logged in
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
        {
            if (it->second.accountId == account->accountId)
            {
                // Disconnect existing session
                account->totalPlayTimeHours += it->second.sessionDuration / 3600.0f;
                m_sessions.erase(it);
                break;
            }
        }

        // Create session
        SessionData session;
        session.sessionToken = GenerateSessionToken();
        session.accountId = account->accountId;
        session.username = username;
        session.tier = account->tier;
        session.loginTime = now;
        session.lastActivity = now;
        session.authenticated = true;

        uint64_t token = session.sessionToken;
        m_sessions[token] = std::move(session);

        // Update account
        account->lastLogin = now;
        account->failedLoginAttempts = 0;

        LoginRecord record;
        record.timestamp = now;
        record.success = true;
        account->loginHistory.push_back(record);
        if (account->loginHistory.size() > MAX_LOGIN_HISTORY)
            account->loginHistory.erase(account->loginHistory.begin());

        result.success = true;
        result.accountId = account->accountId;
        result.sessionToken = token;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Login: %s", username.c_str());
        return result;
    }

    void MMOAccountSystem::Logout(uint64_t sessionToken)
    {
        auto it = m_sessions.find(sessionToken);
        if (it == m_sessions.end())
            return;

        auto acctIt = m_accounts.find(it->second.accountId);
        if (acctIt != m_accounts.end())
            acctIt->second.totalPlayTimeHours += it->second.sessionDuration / 3600.0f;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Logout: %s", it->second.username.c_str());
        m_sessions.erase(it);
    }

    bool MMOAccountSystem::ValidateSession(uint64_t sessionToken) const
    {
        auto it = m_sessions.find(sessionToken);
        return it != m_sessions.end() && it->second.authenticated;
    }

    const SessionData* MMOAccountSystem::GetSession(uint64_t sessionToken) const
    {
        auto it = m_sessions.find(sessionToken);
        return it != m_sessions.end() ? &it->second : nullptr;
    }

    SessionData* MMOAccountSystem::GetSessionMut(uint64_t sessionToken)
    {
        auto it = m_sessions.find(sessionToken);
        return it != m_sessions.end() ? &it->second : nullptr;
    }

    const SessionData* MMOAccountSystem::GetSessionByAccount(uint32_t accountId) const
    {
        for (const auto& [token, session] : m_sessions)
        {
            if (session.accountId == accountId)
                return &session;
        }
        return nullptr;
    }

    bool MMOAccountSystem::SetActiveCharacter(uint64_t sessionToken, uint32_t characterId)
    {
        auto* session = GetSessionMut(sessionToken);
        if (!session)
            return false;
        session->activeCharacterId = characterId;
        session->lastActivity = GetTimestamp();
        return true;
    }

    // === Account Management ===

    const AccountData* MMOAccountSystem::GetAccount(uint32_t accountId) const
    {
        auto it = m_accounts.find(accountId);
        return it != m_accounts.end() ? &it->second : nullptr;
    }

    const AccountData* MMOAccountSystem::FindAccount(const std::string& username) const
    {
        for (const auto& [id, acct] : m_accounts)
        {
            if (acct.username == username)
                return &acct;
        }
        return nullptr;
    }

    bool MMOAccountSystem::ChangePassword(uint32_t accountId, const std::string& oldPass, const std::string& newPass)
    {
        auto it = m_accounts.find(accountId);
        if (it == m_accounts.end())
            return false;

        auto& acct = it->second;
        if (HashPassword(oldPass, acct.salt) != acct.passwordHash)
            return false;

        if (newPass.size() < 6)
            return false;

        acct.salt = GenerateSalt();
        acct.passwordHash = HashPassword(newPass, acct.salt);
        return true;
    }

    bool MMOAccountSystem::SetAccountStatus(uint32_t accountId, AccountStatus status, const std::string& reason)
    {
        auto it = m_accounts.find(accountId);
        if (it == m_accounts.end())
            return false;
        it->second.status = status;
        if (status == AccountStatus::Banned || status == AccountStatus::Suspended)
            it->second.banReason = reason;
        return true;
    }

    bool MMOAccountSystem::BanAccount(uint32_t accountId, float durationHours, const std::string& reason)
    {
        auto it = m_accounts.find(accountId);
        if (it == m_accounts.end())
            return false;

        it->second.status = AccountStatus::Banned;
        it->second.banReason = reason;
        it->second.banExpiry =
            durationHours > 0 ? GetTimestamp() + static_cast<uint64_t>(durationHours * 3600) : 0; // 0 = permanent

        // Kick active session
        for (auto sit = m_sessions.begin(); sit != m_sessions.end(); ++sit)
        {
            if (sit->second.accountId == accountId)
            {
                m_sessions.erase(sit);
                break;
            }
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[MMO] Account banned: %s (%s)", it->second.username.c_str(),
                       reason.c_str());
        return true;
    }

    bool MMOAccountSystem::UnbanAccount(uint32_t accountId)
    {
        auto it = m_accounts.find(accountId);
        if (it == m_accounts.end())
            return false;
        it->second.status = AccountStatus::Active;
        it->second.banExpiry = 0;
        it->second.banReason.clear();
        it->second.failedLoginAttempts = 0;
        return true;
    }

    int MMOAccountSystem::GetCharacterSlots(uint32_t accountId) const
    {
        const auto* acct = GetAccount(accountId);
        return acct ? acct->maxCharacterSlots : 0;
    }

    bool MMOAccountSystem::IsLoggedIn(uint32_t accountId) const
    {
        return GetSessionByAccount(accountId) != nullptr;
    }

    // === Cleanup ===

    void MMOAccountSystem::CleanExpiredSessions()
    {
        uint64_t now = GetTimestamp();
        for (auto it = m_sessions.begin(); it != m_sessions.end();)
        {
            float idleTime = static_cast<float>(now - it->second.lastActivity);
            if (idleTime > SESSION_TIMEOUT)
            {
                auto acctIt = m_accounts.find(it->second.accountId);
                if (acctIt != m_accounts.end())
                    acctIt->second.totalPlayTimeHours += it->second.sessionDuration / 3600.0f;
                it = m_sessions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void MMOAccountSystem::CleanExpiredBans()
    {
        uint64_t now = GetTimestamp();
        for (auto& [id, acct] : m_accounts)
        {
            if (acct.status == AccountStatus::Banned && acct.banExpiry > 0 && acct.banExpiry <= now)
            {
                acct.status = AccountStatus::Active;
                acct.banExpiry = 0;
                acct.banReason.clear();
            }
            if (acct.status == AccountStatus::Locked)
            {
                // Auto-unlock after lockout duration
                if (acct.lastLogin > 0 && (now - acct.lastLogin) > static_cast<uint64_t>(LOCKOUT_DURATION))
                {
                    acct.status = AccountStatus::Active;
                    acct.failedLoginAttempts = 0;
                }
            }
        }
    }

    // === String output ===

    std::string MMOAccountSystem::GetAccountInfoString(uint32_t accountId) const
    {
        const auto* acct = GetAccount(accountId);
        if (!acct)
            return "Account not found";

        std::ostringstream ss;
        ss << "Account: " << acct->username << " (ID " << acct->accountId << ")\n";
        ss << "Status: " << static_cast<int>(acct->status) << "\n";
        ss << "Tier: " << static_cast<int>(acct->tier) << "\n";
        ss << "Character slots: " << acct->maxCharacterSlots << "\n";
        ss << "Play time: " << acct->totalPlayTimeHours << "h\n";
        ss << "Online: " << (IsLoggedIn(accountId) ? "Yes" : "No") << "\n";
        return ss.str();
    }

    std::string MMOAccountSystem::GetOnlineListString() const
    {
        std::ostringstream ss;
        ss << "Online Players (" << m_sessions.size() << "):\n";
        for (const auto& [token, session] : m_sessions)
        {
            ss << "  " << session.username << " (Acct " << session.accountId << ") - "
               << static_cast<int>(session.sessionDuration) << "s\n";
        }
        return ss.str();
    }

    void MMOAccountSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Account System"))
        {
            ImGui::Text("Accounts: %zu | Online: %zu", m_accounts.size(), m_sessions.size());

            if (ImGui::TreeNode("Online Sessions"))
            {
                for (const auto& [token, session] : m_sessions)
                {
                    ImGui::Text("  %s (Acct#%u) %.0fs | Char#%u", session.username.c_str(), session.accountId,
                                session.sessionDuration, session.activeCharacterId);
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("All Accounts"))
            {
                for (const auto& [id, acct] : m_accounts)
                {
                    ImGui::Text("  [%u] %s (Status:%d Tier:%d Playtime:%.1fh)", id, acct.username.c_str(),
                                static_cast<int>(acct.status), static_cast<int>(acct.tier), acct.totalPlayTimeHours);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
