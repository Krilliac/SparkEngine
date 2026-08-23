/**
 * @file MMOAccountSystem.cpp
 * @brief Account registration, login authentication, and session management
 */

#include "MMOAccountSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/PasswordHash.h"
#include "Utils/SecureRandom.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <chrono>
#include <sstream>

namespace MMO
{

    bool MMOAccountSystem::Initialize(Spark::IEngineContext* context)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_context = context;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO account system initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Account system initialized");
        return true;
    }

    void MMOAccountSystem::Update(float dt)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

    std::string MMOAccountSystem::GenerateSessionToken()
    {
        return Spark::SecureRandom::HexToken(16);
    }

    uint64_t MMOAccountSystem::GetTimestamp()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // === Registration ===

    AuthResult MMOAccountSystem::Register(const std::string& username, std::string_view password,
                                          const std::string& email)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        AuthResult result;

        // Validate input
        if (username.size() < 3 || username.size() > 24)
        {
            result.errorMessage = "Username must be 3-24 characters";
            return result;
        }
        if (password.size() < 6 || password.size() > 1024)
        {
            result.errorMessage = "Password must be 6-1024 characters";
            return result;
        }

        // Check uniqueness
        if (FindAccount(username))
        {
            result.errorMessage = "Username already taken";
            return result;
        }

        // Create account
        const std::string passwordHash = Spark::PasswordHash::Create(password);
        if (passwordHash.empty())
        {
            result.errorMessage = "Secure password hashing is unavailable";
            return result;
        }

        AccountData account;
        account.accountId = m_nextAccountId;
        account.username = username;
        account.salt.clear(); // Salt and parameters are embedded in the self-describing hash.
        account.passwordHash = passwordHash;
        account.email = email;
        account.status = AccountStatus::Active;
        account.tier = AccountTier::Free;
        account.createdAt = GetTimestamp();

        uint32_t id = account.accountId;
        m_accounts[id] = std::move(account);
        ++m_nextAccountId;

        result.success = true;
        result.accountId = id;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Account registered: %s (ID %u)", username.c_str(), id);
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Account registered: " + username + " (ID " +
                                                    std::to_string(id) + ")");
        return result;
    }

    // === Authentication ===

    AuthResult MMOAccountSystem::Login(const std::string& username, std::string_view password)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
        if (!Spark::PasswordHash::Verify(password, account->passwordHash))
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

        // Generate a collision-free 128-bit bearer token before mutating the
        // current session. CSPRNG failure leaves an existing login untouched.
        std::string token;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            std::string candidate = GenerateSessionToken();
            if (candidate.empty())
                break;
            if (!m_sessions.contains(candidate))
            {
                token = std::move(candidate);
                break;
            }
        }
        if (token.empty())
        {
            result.errorMessage = "Unable to create a secure session";
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
        session.sessionToken = token;
        session.accountId = account->accountId;
        session.username = username;
        session.tier = account->tier;
        session.loginTime = now;
        session.lastActivity = now;
        session.authenticated = true;

        const bool inserted = m_sessions.emplace(token, std::move(session)).second;
        if (!inserted)
        {
            result.errorMessage = "Unable to create a unique session";
            return result;
        }

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

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Account login: %s (ID %u)", username.c_str(), account->accountId);
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Login: " + username);
        return result;
    }

    void MMOAccountSystem::Logout(const std::string& sessionToken)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_sessions.find(sessionToken);
        if (it == m_sessions.end())
            return;

        auto acctIt = m_accounts.find(it->second.accountId);
        if (acctIt != m_accounts.end())
            acctIt->second.totalPlayTimeHours += it->second.sessionDuration / 3600.0f;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Account logout: %s", it->second.username.c_str());
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Logout: " + it->second.username);
        m_sessions.erase(it);
    }

    bool MMOAccountSystem::ValidateSession(const std::string& sessionToken) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_sessions.find(sessionToken);
        return it != m_sessions.end() && it->second.authenticated;
    }

    std::optional<SessionData> MMOAccountSystem::GetSession(const std::string& sessionToken) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_sessions.find(sessionToken);
        return it != m_sessions.end() ? std::optional<SessionData>(it->second) : std::nullopt;
    }

    std::optional<SessionData> MMOAccountSystem::GetSessionByAccount(uint32_t accountId) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& [token, session] : m_sessions)
        {
            if (session.accountId == accountId)
                return session;
        }
        return std::nullopt;
    }

    bool MMOAccountSystem::SetActiveCharacter(const std::string& sessionToken, uint32_t characterId)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto session = m_sessions.find(sessionToken);
        if (session == m_sessions.end())
            return false;
        session->second.activeCharacterId = characterId;
        session->second.lastActivity = GetTimestamp();
        return true;
    }

    // === Account Management ===

    std::optional<AccountData> MMOAccountSystem::GetAccount(uint32_t accountId) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_accounts.find(accountId);
        return it != m_accounts.end() ? std::optional<AccountData>(it->second) : std::nullopt;
    }

    std::optional<AccountData> MMOAccountSystem::FindAccount(const std::string& username) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& [id, acct] : m_accounts)
        {
            if (acct.username == username)
                return acct;
        }
        return std::nullopt;
    }

    bool MMOAccountSystem::ChangePassword(uint32_t accountId, std::string_view oldPass, std::string_view newPass)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_accounts.find(accountId);
        if (it == m_accounts.end())
            return false;

        auto& acct = it->second;
        if (!Spark::PasswordHash::Verify(oldPass, acct.passwordHash))
            return false;

        if (newPass.size() < 6 || newPass.size() > 1024)
            return false;

        const std::string passwordHash = Spark::PasswordHash::Create(newPass);
        if (passwordHash.empty())
            return false;
        acct.salt.clear();
        acct.passwordHash = passwordHash;
        return true;
    }

    bool MMOAccountSystem::SetAccountStatus(uint32_t accountId, AccountStatus status, const std::string& reason)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

        SPARK_LOG_WARN(Spark::LogCategory::Game, "Account banned: %s (%s)", it->second.username.c_str(),
                       reason.c_str());
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Account banned: " + it->second.username + " (" + reason +
                                                    ")");
        return true;
    }

    bool MMOAccountSystem::UnbanAccount(uint32_t accountId)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto acct = GetAccount(accountId);
        return acct ? acct->maxCharacterSlots : 0;
    }

    bool MMOAccountSystem::IsLoggedIn(uint32_t accountId) const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return GetSessionByAccount(accountId).has_value();
    }

    // === Cleanup ===

    void MMOAccountSystem::CleanExpiredSessions()
    {
        uint64_t now = GetTimestamp();
        std::erase_if(m_sessions,
                      [&](const auto& entry)
                      {
                          const auto& session = entry.second;
                          float idleTime = static_cast<float>(now - session.lastActivity);
                          if (idleTime > SESSION_TIMEOUT)
                          {
                              auto acctIt = m_accounts.find(session.accountId);
                              if (acctIt != m_accounts.end())
                                  acctIt->second.totalPlayTimeHours += session.sessionDuration / 3600.0f;
                              return true;
                          }
                          return false;
                      });
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const auto acct = GetAccount(accountId);
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
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

    size_t MMOAccountSystem::GetOnlineCount() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_sessions.size();
    }

    size_t MMOAccountSystem::GetAccountCount() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_accounts.size();
    }

} // namespace MMO
