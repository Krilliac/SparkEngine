/**
 * @file OnlineServices.h
 * @brief Unified online platform integration with pluggable backends
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides an abstract interface for online platform services (authentication,
 * matchmaking, leaderboards, achievements, cloud saves, friends/presence)
 * with a fully functional offline implementation and documented stubs for
 * Steam, Epic, PlayStation, and Xbox integration.
 *
 * ## Architecture
 * ```
 * OnlineServiceManager (singleton)
 *   └── IOnlinePlatform* (active platform)
 *         ├── NullOnlinePlatform  (default — fully functional offline)
 *         ├── SteamPlatform       (stub — requires Steamworks SDK)
 *         ├── EpicPlatform        (stub — requires EOS SDK)
 *         └── ConsolePlatform     (stub — requires NDA + dev kits)
 * ```
 *
 * ## Usage
 * @code
 *   auto& online = Spark::OnlineServices::OnlineServiceManager::GetInstance();
 *   online.Initialize();  // Uses NullOnlinePlatform by default
 *
 *   auto* platform = online.GetPlatform();
 *   platform->Login("player1", "");
 *   platform->SubmitScore("HighScores", 9999);
 *   platform->UnlockAchievement("first_kill");
 *   platform->SaveToCloud("save1", saveData);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "Utils/LogMacros.h"

namespace Spark::OnlineServices
{

    // ========================================================================
    // Data structures
    // ========================================================================

    /** @brief Player identification */
    struct OnlinePlayerInfo
    {
        std::string playerId;    ///< Platform-specific unique ID
        std::string displayName; ///< Human-readable display name
        bool isOnline = false;   ///< Current online status
    };

    /** @brief Multiplayer session information */
    struct SessionInfo
    {
        std::string sessionId;       ///< Unique session ID
        std::string hostName;        ///< Host player name
        std::string mapName;         ///< Current map/level
        std::string gameMode;        ///< Game mode (e.g. "Deathmatch")
        uint32_t currentPlayers = 0; ///< Current player count
        uint32_t maxPlayers = 0;     ///< Maximum players
        uint32_t ping = 0;           ///< Latency in ms
        bool isPublic = true;        ///< Whether the session is publicly visible
    };

    /** @brief Leaderboard score entry */
    struct LeaderboardEntry
    {
        std::string playerId;
        std::string playerName;
        int64_t score = 0;
        uint32_t rank = 0;
    };

    /** @brief Achievement data */
    struct AchievementInfo
    {
        std::string id;          ///< Achievement identifier
        std::string name;        ///< Display name
        std::string description; ///< Description text
        float progress = 0.0f;   ///< Progress (0.0 to 1.0)
        bool unlocked = false;   ///< Whether fully unlocked
    };

    /** @brief Cloud save metadata */
    struct CloudSaveInfo
    {
        std::string slotName;   ///< Save slot identifier
        uint64_t sizeBytes = 0; ///< Save data size
        std::string timestamp;  ///< Last modified timestamp
    };

    /** @brief Friend list entry */
    struct FriendInfo
    {
        std::string playerId;
        std::string displayName;
        bool isOnline = false;
        std::string presence; ///< Status text (e.g. "In Game — Level 5")
    };

    /** @brief Feature capability mask for a platform backend */
    struct PlatformCapabilities
    {
        bool authentication = false;
        bool sessions = false;
        bool leaderboards = false;
        bool achievements = false;
        bool cloudSave = false;
        bool friends = false;
        bool presence = false;
    };

    // ========================================================================
    // Abstract platform interface
    // ========================================================================

    /**
     * @brief Abstract interface for online platform services
     *
     * Implement this interface for each target platform (Steam, Epic, PSN, Xbox).
     * The NullOnlinePlatform provides a fully functional offline implementation.
     */
    class IOnlinePlatform
    {
      public:
        virtual ~IOnlinePlatform() = default;

        /** @brief Get the platform name (e.g. "Steam", "Null") */
        virtual std::string GetPlatformName() const = 0;
        /** @brief Query which online features the active backend supports. */
        virtual PlatformCapabilities GetCapabilities() const = 0;
        /** @brief Last human-readable failure reason from this backend (empty if none). */
        virtual std::string GetLastError() const = 0;

        // --- Authentication ---
        virtual bool Login(const std::string& username, const std::string& token) = 0;
        virtual void Logout() = 0;
        virtual bool IsLoggedIn() const = 0;
        virtual OnlinePlayerInfo GetLocalPlayer() const = 0;

        // --- Matchmaking ---
        virtual std::vector<SessionInfo> FindSessions(const std::string& filter = "") = 0;
        virtual bool CreateSession(const SessionInfo& settings) = 0;
        virtual bool JoinSession(const std::string& sessionId) = 0;
        virtual void LeaveSession() = 0;
        virtual SessionInfo GetCurrentSession() const = 0;

        // --- Leaderboards ---
        virtual bool SubmitScore(const std::string& boardName, int64_t score) = 0;
        virtual std::vector<LeaderboardEntry> QueryScores(const std::string& boardName, uint32_t maxResults = 10) = 0;

        // --- Achievements ---
        virtual bool UnlockAchievement(const std::string& achievementId) = 0;
        virtual bool SetAchievementProgress(const std::string& id, float progress) = 0;
        virtual std::vector<AchievementInfo> QueryAchievements() = 0;

        // --- Cloud Saves ---
        virtual bool SaveToCloud(const std::string& slotName, const std::vector<uint8_t>& data) = 0;
        virtual std::vector<uint8_t> LoadFromCloud(const std::string& slotName) = 0;
        virtual bool DeleteCloudSave(const std::string& slotName) = 0;
        virtual std::vector<CloudSaveInfo> ListCloudSaves() = 0;

        // --- Friends / Presence ---
        virtual std::vector<FriendInfo> GetFriendsList() = 0;
        virtual bool SetPresence(const std::string& statusText) = 0;
        virtual bool InviteToSession(const std::string& friendId) = 0;
    };

    // ========================================================================
    // Null (offline) implementation — fully functional
    // ========================================================================

    /**
     * @brief Fully functional offline platform implementation
     *
     * Stores leaderboards in memory, achievements as a set, and cloud saves
     * to a local directory. Used as the default platform and for single-player
     * games that don't need online services.
     */
    class NullOnlinePlatform : public IOnlinePlatform
    {
      public:
        std::string GetPlatformName() const override { return "Null (Offline)"; }
        PlatformCapabilities GetCapabilities() const override
        {
            PlatformCapabilities c;
            c.authentication = true;
            c.sessions = true;
            c.leaderboards = true;
            c.achievements = true;
            c.cloudSave = true;
            c.friends = true;
            c.presence = true;
            return c;
        }
        std::string GetLastError() const override { return m_lastError; }

        bool Login(const std::string& username, const std::string& /*token*/) override
        {
            m_player.playerId = "local_" + username;
            m_player.displayName = username;
            m_player.isOnline = true;
            m_loggedIn = true;
            m_lastError.clear();
            SPARK_LOG_INFO(Spark::LogCategory::Network, "Online: Login as '%s' (offline mode)", username.c_str());
            return true;
        }

        void Logout() override
        {
            SPARK_LOG_INFO(Spark::LogCategory::Network, "Online: Logout (offline mode)");
            m_loggedIn = false;
            m_player.isOnline = false;
        }

        bool IsLoggedIn() const override { return m_loggedIn; }
        OnlinePlayerInfo GetLocalPlayer() const override { return m_player; }

        std::vector<SessionInfo> FindSessions(const std::string& /*filter*/) override
        {
            m_lastError.clear();
            return m_sessions;
        }

        bool CreateSession(const SessionInfo& settings) override
        {
            m_lastError.clear();
            m_currentSession = settings;
            m_currentSession.sessionId = "local_" + std::to_string(m_nextSessionId++);
            m_sessions.push_back(m_currentSession);
            return true;
        }

        bool JoinSession(const std::string& sessionId) override
        {
            m_lastError.clear();
            for (auto& s : m_sessions)
            {
                if (s.sessionId == sessionId)
                {
                    m_currentSession = s;
                    return true;
                }
            }
            m_lastError = "Session not found: " + sessionId;
            return false;
        }

        void LeaveSession() override { m_currentSession = {}; }
        SessionInfo GetCurrentSession() const override { return m_currentSession; }

        bool SubmitScore(const std::string& boardName, int64_t score) override
        {
            SPARK_LOG_INFO(Spark::LogCategory::Network, "Online: SubmitScore '%s' = %lld (offline mode)",
                           boardName.c_str(), static_cast<long long>(score));
            m_lastError.clear();
            auto& board = m_leaderboards[boardName];
            LeaderboardEntry entry;
            entry.playerId = m_player.playerId;
            entry.playerName = m_player.displayName;
            entry.score = score;
            board.push_back(entry);
            // Sort by score descending (ties keep submission order) and assign ranks
            std::stable_sort(board.begin(), board.end(),
                             [](const auto& a, const auto& b) { return a.score > b.score; });
            for (uint32_t i = 0; i < board.size(); ++i)
                board[i].rank = i + 1;
            return true;
        }

        std::vector<LeaderboardEntry> QueryScores(const std::string& boardName, uint32_t maxResults) override
        {
            m_lastError.clear();
            auto it = m_leaderboards.find(boardName);
            if (it == m_leaderboards.end())
                return {};
            auto& board = it->second;
            uint32_t count = std::min(maxResults, static_cast<uint32_t>(board.size()));
            return {board.begin(), board.begin() + count};
        }

        bool UnlockAchievement(const std::string& achievementId) override
        {
            SPARK_LOG_INFO(Spark::LogCategory::Network, "Online: UnlockAchievement '%s' (offline mode)",
                           achievementId.c_str());
            m_lastError.clear();
            m_achievements[achievementId] = 1.0f;
            return true;
        }

        bool SetAchievementProgress(const std::string& id, float progress) override
        {
            m_lastError.clear();
            m_achievements[id] = std::min(1.0f, std::max(0.0f, progress));
            return true;
        }

        std::vector<AchievementInfo> QueryAchievements() override
        {
            m_lastError.clear();
            std::vector<AchievementInfo> result;
            for (const auto& [id, progress] : m_achievements)
            {
                AchievementInfo info;
                info.id = id;
                info.name = id;
                info.progress = progress;
                info.unlocked = (progress >= 1.0f);
                result.push_back(info);
            }
            return result;
        }

        bool SaveToCloud(const std::string& slotName, const std::vector<uint8_t>& data) override
        {
            SPARK_LOG_INFO(Spark::LogCategory::Network, "Online: SaveToCloud slot '%s' (%zu bytes, offline mode)",
                           slotName.c_str(), data.size());
            m_lastError.clear();
            m_cloudSaves[slotName] = data;
            return true;
        }

        std::vector<uint8_t> LoadFromCloud(const std::string& slotName) override
        {
            m_lastError.clear();
            auto it = m_cloudSaves.find(slotName);
            if (it == m_cloudSaves.end())
            {
                m_lastError = "Cloud slot not found: " + slotName;
                return {};
            }
            return it->second;
        }

        bool DeleteCloudSave(const std::string& slotName) override
        {
            m_lastError.clear();
            if (m_cloudSaves.erase(slotName) == 0)
            {
                m_lastError = "Cannot delete missing cloud slot: " + slotName;
                return false;
            }
            return true;
        }

        std::vector<CloudSaveInfo> ListCloudSaves() override
        {
            m_lastError.clear();
            std::vector<CloudSaveInfo> result;
            for (const auto& [name, data] : m_cloudSaves)
                result.push_back({name, data.size(), ""});
            return result;
        }

        std::vector<FriendInfo> GetFriendsList() override
        {
            m_lastError.clear();
            return m_friends;
        }

        bool SetPresence(const std::string& statusText) override
        {
            m_lastError.clear();
            m_presence = statusText;
            return true;
        }

        bool InviteToSession(const std::string& friendId) override
        {
            // An invite needs a known recipient and a session to invite into; reporting
            // success otherwise would fabricate an outcome the caller cannot observe.
            // Offline mode has no friends list, so every invite fails with a reason.
            m_lastError.clear();
            if (friendId.empty())
            {
                m_lastError = "Invite requires a friend ID";
                return false;
            }
            if (m_currentSession.sessionId.empty())
            {
                m_lastError = "Invite requires an active session";
                return false;
            }
            const bool isFriend = std::any_of(m_friends.begin(), m_friends.end(),
                                              [&](const FriendInfo& f) { return f.playerId == friendId; });
            if (!isFriend)
            {
                m_lastError = "Invite recipient is not a friend: " + friendId;
                return false;
            }
            return true;
        }

      private:
        bool m_loggedIn = false;
        std::string m_lastError;
        OnlinePlayerInfo m_player;
        SessionInfo m_currentSession;
        std::vector<SessionInfo> m_sessions;
        uint32_t m_nextSessionId = 1;
        // Ordered containers so query and list results come back in the same (sorted-key)
        // order on every standard library and every run.
        std::map<std::string, std::vector<LeaderboardEntry>> m_leaderboards;
        std::map<std::string, float> m_achievements;
        std::map<std::string, std::vector<uint8_t>> m_cloudSaves;
        std::vector<FriendInfo> m_friends;
        std::string m_presence;
    };

    // ========================================================================
    // Steam stub (requires Steamworks SDK)
    // ========================================================================

    /**
     * @brief Steam platform stub — requires Steamworks SDK for full implementation
     *
     * ## Integration Guide
     *
     * ### Obtaining the SDK
     * 1. Register at https://partner.steamgames.com
     * 2. Download the Steamworks SDK from the partner portal
     * 3. Place the SDK in `ThirdParty/Steamworks/`
     * 4. Add `-DENABLE_STEAM=ON` to CMake
     *
     * ### Required Headers
     * - `steam/steam_api.h` — Main API entry point
     * - `steam/isteamuserstats.h` — Leaderboards and achievements
     * - `steam/isteammatchmaking.h` — Session/lobby management
     * - `steam/isteamremotestorage.h` — Cloud saves
     * - `steam/isteamfriends.h` — Friends list and presence
     *
     * ### Initialization Pattern
     * ```cpp
     * bool SteamPlatform::Login(const std::string&, const std::string&) {
     *     if (!SteamAPI_Init()) return false;
     *     // SteamUser()->GetSteamID() for player ID
     *     // SteamFriends()->GetPersonaName() for display name
     *     return true;
     * }
     * ```
     *
     * ### Async Callback Pattern
     * Steam uses a callback system. In Update(), call `SteamAPI_RunCallbacks()`.
     * Register callbacks with `STEAM_CALLBACK(Class, OnCallback, CallbackType)`.
     *
     * ### Leaderboards
     * ```cpp
     * // SteamUserStats()->FindLeaderboard("name") → SteamAPICall_t
     * // On result: SteamUserStats()->UploadLeaderboardScore(handle, method, score)
     * ```
     *
     * ### Achievements
     * ```cpp
     * // SteamUserStats()->SetAchievement("achievement_id")
     * // SteamUserStats()->StoreStats() — must call to persist
     * ```
     */
    class SteamPlatform : public IOnlinePlatform
    {
      public:
        std::string GetPlatformName() const override { return "Steam (Stub)"; }
        PlatformCapabilities GetCapabilities() const override { return {}; }
        std::string GetLastError() const override { return "Steamworks SDK unavailable in this build"; }
        bool Login(const std::string&, const std::string&) override { return false; /* SteamAPI_Init() */ }
        void Logout() override { /* SteamAPI_Shutdown() */ }
        bool IsLoggedIn() const override { return false; }
        OnlinePlayerInfo GetLocalPlayer() const override { return {}; }
        std::vector<SessionInfo> FindSessions(const std::string&) override { return {}; }
        bool CreateSession(const SessionInfo&) override { return false; }
        bool JoinSession(const std::string&) override { return false; }
        void LeaveSession() override {}
        SessionInfo GetCurrentSession() const override { return {}; }
        bool SubmitScore(const std::string&, int64_t) override { return false; }
        std::vector<LeaderboardEntry> QueryScores(const std::string&, uint32_t) override { return {}; }
        bool UnlockAchievement(const std::string&) override { return false; }
        bool SetAchievementProgress(const std::string&, float) override { return false; }
        std::vector<AchievementInfo> QueryAchievements() override { return {}; }
        bool SaveToCloud(const std::string&, const std::vector<uint8_t>&) override { return false; }
        std::vector<uint8_t> LoadFromCloud(const std::string&) override { return {}; }
        bool DeleteCloudSave(const std::string&) override { return false; }
        std::vector<CloudSaveInfo> ListCloudSaves() override { return {}; }
        std::vector<FriendInfo> GetFriendsList() override { return {}; }
        bool SetPresence(const std::string&) override { return false; }
        bool InviteToSession(const std::string&) override { return false; }
    };

    /**
     * @brief Epic Online Services stub — requires EOS SDK
     *
     * ## Integration Guide
     *
     * ### Obtaining the SDK
     * 1. Create account at https://dev.epicgames.com
     * 2. Download EOS SDK from the Developer Portal
     * 3. Place in `ThirdParty/EOS/`
     * 4. Add `-DENABLE_EOS=ON` to CMake
     *
     * ### Key APIs
     * - `EOS_Platform_Create()` — Initialize the platform
     * - `EOS_Auth_Login()` — Authentication
     * - `EOS_Lobby_*` — Matchmaking and lobbies
     * - `EOS_Leaderboards_*` — Leaderboard operations
     * - `EOS_Achievements_*` — Achievement tracking
     * - `EOS_PlayerDataStorage_*` — Cloud save storage
     * - `EOS_Friends_*` — Social features
     * - `EOS_Presence_*` — Online presence
     *
     * ### Async Pattern
     * EOS uses completion callbacks. Pass a callback to each API call;
     * call `EOS_Platform_Tick()` in Update() to process results.
     */
    class EpicPlatform : public IOnlinePlatform
    {
      public:
        std::string GetPlatformName() const override { return "Epic (Stub)"; }
        PlatformCapabilities GetCapabilities() const override { return {}; }
        std::string GetLastError() const override { return "EOS SDK unavailable in this build"; }
        bool Login(const std::string&, const std::string&) override { return false; }
        void Logout() override {}
        bool IsLoggedIn() const override { return false; }
        OnlinePlayerInfo GetLocalPlayer() const override { return {}; }
        std::vector<SessionInfo> FindSessions(const std::string&) override { return {}; }
        bool CreateSession(const SessionInfo&) override { return false; }
        bool JoinSession(const std::string&) override { return false; }
        void LeaveSession() override {}
        SessionInfo GetCurrentSession() const override { return {}; }
        bool SubmitScore(const std::string&, int64_t) override { return false; }
        std::vector<LeaderboardEntry> QueryScores(const std::string&, uint32_t) override { return {}; }
        bool UnlockAchievement(const std::string&) override { return false; }
        bool SetAchievementProgress(const std::string&, float) override { return false; }
        std::vector<AchievementInfo> QueryAchievements() override { return {}; }
        bool SaveToCloud(const std::string&, const std::vector<uint8_t>&) override { return false; }
        std::vector<uint8_t> LoadFromCloud(const std::string&) override { return {}; }
        bool DeleteCloudSave(const std::string&) override { return false; }
        std::vector<CloudSaveInfo> ListCloudSaves() override { return {}; }
        std::vector<FriendInfo> GetFriendsList() override { return {}; }
        bool SetPresence(const std::string&) override { return false; }
        bool InviteToSession(const std::string&) override { return false; }
    };

    /**
     * @brief Console platform stub — requires NDA and dev kits
     *
     * ## PlayStation Integration
     * - Apply at https://partners.playstation.net
     * - Requires NDA and approved developer status
     * - SDK: PlayStation Partners SDK (NDA-protected)
     * - APIs: SceNpTrophy (achievements), SceNpMatching2 (matchmaking),
     *   SceNpWebApi (leaderboards), SceSaveData (cloud saves)
     *
     * ## Xbox Integration
     * - Apply at https://developer.microsoft.com/en-us/games
     * - Requires ID@Xbox or managed partner approval
     * - SDK: GDK (Gaming Development Kit)
     * - APIs: XGameSave (cloud saves), XblAchievements,
     *   XblMultiplayer (matchmaking), XblSocial (friends)
     *
     * ## Nintendo Switch Integration
     * - Apply at https://developer.nintendo.com
     * - Requires approved developer status
     * - SDK: Nintendo Switch SDK (NDA-protected)
     */
    class ConsolePlatform : public IOnlinePlatform
    {
      public:
        std::string GetPlatformName() const override { return "Console (Stub)"; }
        PlatformCapabilities GetCapabilities() const override { return {}; }
        std::string GetLastError() const override
        {
            return "Console SDK unavailable in this build (NDA platform integration required)";
        }
        bool Login(const std::string&, const std::string&) override { return false; }
        void Logout() override {}
        bool IsLoggedIn() const override { return false; }
        OnlinePlayerInfo GetLocalPlayer() const override { return {}; }
        std::vector<SessionInfo> FindSessions(const std::string&) override { return {}; }
        bool CreateSession(const SessionInfo&) override { return false; }
        bool JoinSession(const std::string&) override { return false; }
        void LeaveSession() override {}
        SessionInfo GetCurrentSession() const override { return {}; }
        bool SubmitScore(const std::string&, int64_t) override { return false; }
        std::vector<LeaderboardEntry> QueryScores(const std::string&, uint32_t) override { return {}; }
        bool UnlockAchievement(const std::string&) override { return false; }
        bool SetAchievementProgress(const std::string&, float) override { return false; }
        std::vector<AchievementInfo> QueryAchievements() override { return {}; }
        bool SaveToCloud(const std::string&, const std::vector<uint8_t>&) override { return false; }
        std::vector<uint8_t> LoadFromCloud(const std::string&) override { return {}; }
        bool DeleteCloudSave(const std::string&) override { return false; }
        std::vector<CloudSaveInfo> ListCloudSaves() override { return {}; }
        std::vector<FriendInfo> GetFriendsList() override { return {}; }
        bool SetPresence(const std::string&) override { return false; }
        bool InviteToSession(const std::string&) override { return false; }
    };

    // ========================================================================
    // Degraded-dependency accounting (docs/specs/online-services.md section 5.1)
    // ========================================================================

    /** @brief Capability an IOnlinePlatform call belongs to, for failure accounting */
    enum class OnlineCapability : uint8_t
    {
        Authentication,
        Sessions,
        Leaderboards,
        Achievements,
        CloudSave,
        Friends,
        Presence,
        Count
    };

    /** @brief Lower-case capability name used in status output */
    inline const char* OnlineCapabilityName(const OnlineCapability capability)
    {
        switch (capability)
        {
        case OnlineCapability::Authentication:
            return "authentication";
        case OnlineCapability::Sessions:
            return "sessions";
        case OnlineCapability::Leaderboards:
            return "leaderboards";
        case OnlineCapability::Achievements:
            return "achievements";
        case OnlineCapability::CloudSave:
            return "cloudSave";
        case OnlineCapability::Friends:
            return "friends";
        case OnlineCapability::Presence:
            return "presence";
        case OnlineCapability::Count:
            break;
        }
        return "unknown";
    }

    /** @brief Circuit-breaker budget from the spec: 5 consecutive failures open the circuit for 30 s */
    struct OnlineCircuitPolicy
    {
        uint32_t failureThreshold = 5; ///< Consecutive failures that open a capability's circuit
        double cooldownSeconds = 30.0; ///< Time an open circuit rejects calls before one probe is allowed
    };

    /** @brief Failure accounting for one capability of the active adapter */
    struct OnlineCapabilityHealth
    {
        uint32_t consecutiveFailures = 0; ///< Failures since the last success (reset by a success)
        uint64_t totalFailures = 0;       ///< Failed or throwing calls since the adapter was attached
        uint64_t rejectedCalls = 0;       ///< Calls failed immediately because the circuit was open
        bool circuitOpen = false;         ///< True while calls fail fast (a probe is allowed after the cooldown)
        double retryAtSeconds = 0.0;      ///< Manager clock time at which a probe call is allowed
    };

    /**
     * @brief IOnlinePlatform front that OnlineServiceManager::GetPlatform() returns
     *
     * Forwards every call to the attached adapter and applies the section 5.1 failure
     * semantics the adapters cannot enforce themselves:
     * - An exception thrown by the adapter becomes a failed call with a reason; it never
     *   reaches the caller.
     * - Each capability counts consecutive failures. After OnlineCircuitPolicy::failureThreshold
     *   of them its circuit opens, and calls fail immediately without reaching the adapter until
     *   the cooldown has elapsed on the manager clock. The next call is then a probe: success
     *   closes the circuit, failure reopens it for another cooldown.
     * - Logout() and LeaveSession() always reach the adapter so local cleanup is never blocked.
     *
     * A mutation fails when it returns false. A query fails when it throws, or when it returns
     * nothing and the adapter reports a GetLastError() reason for that call.
     *
     * The circuit is disabled for the built-in NullOnlinePlatform: it is in-process and has no
     * remote dependency that can degrade, so its failures (such as an unknown session ID) are
     * caller errors that must not lock out later valid calls. Failures are still counted.
     */
    class GuardedOnlinePlatform final : public IOnlinePlatform
    {
      public:
        /** @brief Attach an adapter (non-owning) and reset all accounting */
        void Attach(IOnlinePlatform* target, const bool circuitEnabled)
        {
            m_target = target;
            m_circuitEnabled = circuitEnabled;
            m_health = {};
            m_guardError.clear();
        }

        /** @brief Advance the clock that open circuits cool down on */
        void AdvanceTime(const float deltaSeconds)
        {
            if (deltaSeconds > 0.0f && std::isfinite(deltaSeconds))
            {
                m_nowSeconds += deltaSeconds;
            }
        }

        void SetPolicy(const OnlineCircuitPolicy& policy) { m_policy = policy; }
        const OnlineCircuitPolicy& GetPolicy() const { return m_policy; }
        bool IsCircuitEnabled() const { return m_circuitEnabled; }
        double GetNowSeconds() const { return m_nowSeconds; }

        const OnlineCapabilityHealth& GetHealth(const OnlineCapability capability) const
        {
            return m_health[static_cast<size_t>(capability)];
        }

        std::string GetPlatformName() const override
        {
            return GuardConst<std::string>("GetPlatformName", {}, [&] { return m_target->GetPlatformName(); });
        }
        PlatformCapabilities GetCapabilities() const override
        {
            return GuardConst<PlatformCapabilities>("GetCapabilities", {}, [&] { return m_target->GetCapabilities(); });
        }
        std::string GetLastError() const override
        {
            if (!m_guardError.empty())
            {
                return m_guardError;
            }
            return GuardConst<std::string>("GetLastError", {}, [&] { return m_target->GetLastError(); });
        }

        bool Login(const std::string& username, const std::string& token) override
        {
            const bool loggedIn =
                GuardBool(OnlineCapability::Authentication, "Login", [&] { return m_target->Login(username, token); });
            RedactFromGuardError(token);
            return loggedIn;
        }
        void Logout() override
        {
            GuardVoid("Logout", [&] { m_target->Logout(); });
        }
        bool IsLoggedIn() const override
        {
            return GuardConst<bool>("IsLoggedIn", false, [&] { return m_target->IsLoggedIn(); });
        }
        OnlinePlayerInfo GetLocalPlayer() const override
        {
            return GuardConst<OnlinePlayerInfo>("GetLocalPlayer", {}, [&] { return m_target->GetLocalPlayer(); });
        }

        std::vector<SessionInfo> FindSessions(const std::string& filter) override
        {
            return GuardQuery(OnlineCapability::Sessions, "FindSessions",
                              [&] { return m_target->FindSessions(filter); });
        }
        bool CreateSession(const SessionInfo& settings) override
        {
            return GuardBool(OnlineCapability::Sessions, "CreateSession",
                             [&] { return m_target->CreateSession(settings); });
        }
        bool JoinSession(const std::string& sessionId) override
        {
            return GuardBool(OnlineCapability::Sessions, "JoinSession",
                             [&] { return m_target->JoinSession(sessionId); });
        }
        void LeaveSession() override
        {
            GuardVoid("LeaveSession", [&] { m_target->LeaveSession(); });
        }
        SessionInfo GetCurrentSession() const override
        {
            return GuardConst<SessionInfo>("GetCurrentSession", {}, [&] { return m_target->GetCurrentSession(); });
        }

        bool SubmitScore(const std::string& boardName, int64_t score) override
        {
            return GuardBool(OnlineCapability::Leaderboards, "SubmitScore",
                             [&] { return m_target->SubmitScore(boardName, score); });
        }
        std::vector<LeaderboardEntry> QueryScores(const std::string& boardName, uint32_t maxResults) override
        {
            return GuardQuery(OnlineCapability::Leaderboards, "QueryScores",
                              [&] { return m_target->QueryScores(boardName, maxResults); });
        }

        bool UnlockAchievement(const std::string& achievementId) override
        {
            return GuardBool(OnlineCapability::Achievements, "UnlockAchievement",
                             [&] { return m_target->UnlockAchievement(achievementId); });
        }
        bool SetAchievementProgress(const std::string& id, float progress) override
        {
            return GuardBool(OnlineCapability::Achievements, "SetAchievementProgress",
                             [&] { return m_target->SetAchievementProgress(id, progress); });
        }
        std::vector<AchievementInfo> QueryAchievements() override
        {
            return GuardQuery(OnlineCapability::Achievements, "QueryAchievements",
                              [&] { return m_target->QueryAchievements(); });
        }

        bool SaveToCloud(const std::string& slotName, const std::vector<uint8_t>& data) override
        {
            return GuardBool(OnlineCapability::CloudSave, "SaveToCloud",
                             [&] { return m_target->SaveToCloud(slotName, data); });
        }
        std::vector<uint8_t> LoadFromCloud(const std::string& slotName) override
        {
            return GuardQuery(OnlineCapability::CloudSave, "LoadFromCloud",
                              [&] { return m_target->LoadFromCloud(slotName); });
        }
        bool DeleteCloudSave(const std::string& slotName) override
        {
            return GuardBool(OnlineCapability::CloudSave, "DeleteCloudSave",
                             [&] { return m_target->DeleteCloudSave(slotName); });
        }
        std::vector<CloudSaveInfo> ListCloudSaves() override
        {
            return GuardQuery(OnlineCapability::CloudSave, "ListCloudSaves",
                              [&] { return m_target->ListCloudSaves(); });
        }

        std::vector<FriendInfo> GetFriendsList() override
        {
            return GuardQuery(OnlineCapability::Friends, "GetFriendsList", [&] { return m_target->GetFriendsList(); });
        }
        bool SetPresence(const std::string& statusText) override
        {
            return GuardBool(OnlineCapability::Presence, "SetPresence",
                             [&] { return m_target->SetPresence(statusText); });
        }
        bool InviteToSession(const std::string& friendId) override
        {
            return GuardBool(OnlineCapability::Friends, "InviteToSession",
                             [&] { return m_target->InviteToSession(friendId); });
        }

      private:
        OnlineCapabilityHealth& Health(const OnlineCapability capability)
        {
            return m_health[static_cast<size_t>(capability)];
        }

        // Runs one adapter call. Returns false (with m_guardError set) if it threw.
        template <typename Call> bool InvokeAdapter(const char* operation, Call&& call) const
        {
            try
            {
                call();
                return true;
            }
            catch (const std::exception& e)
            {
                m_guardError = std::format("{} failed: adapter threw: {}", operation, e.what());
            }
            catch (...)
            {
                m_guardError = std::format("{} failed: adapter threw a non-standard exception", operation);
            }
            return false;
        }

        // Fails the call without reaching the adapter while the capability's circuit is open.
        bool RejectIfOpen(const OnlineCapability capability, const char* operation)
        {
            OnlineCapabilityHealth& health = Health(capability);
            if (!m_circuitEnabled || !health.circuitOpen || m_nowSeconds >= health.retryAtSeconds)
            {
                return false;
            }
            ++health.rejectedCalls;
            m_guardError = std::format("{} failed: {} circuit open after {} consecutive failures (retry in {:.1f}s)",
                                       operation, OnlineCapabilityName(capability), health.consecutiveFailures,
                                       health.retryAtSeconds - m_nowSeconds);
            return true;
        }

        void RecordOutcome(const OnlineCapability capability, const bool succeeded)
        {
            OnlineCapabilityHealth& health = Health(capability);
            if (succeeded)
            {
                if (health.circuitOpen)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Network, "Online: %s circuit closed after a successful probe",
                                   OnlineCapabilityName(capability));
                }
                health.consecutiveFailures = 0;
                health.circuitOpen = false;
                return;
            }

            ++health.consecutiveFailures;
            ++health.totalFailures;
            if (m_circuitEnabled && health.consecutiveFailures >= m_policy.failureThreshold)
            {
                // Opening, or a failed probe after the cooldown: fail fast for another cooldown.
                health.circuitOpen = true;
                health.retryAtSeconds = m_nowSeconds + m_policy.cooldownSeconds;
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "Online: %s circuit open after %u consecutive failures, retry in %.1fs",
                               OnlineCapabilityName(capability), health.consecutiveFailures, m_policy.cooldownSeconds);
            }
        }

        template <typename Call> bool GuardBool(const OnlineCapability capability, const char* operation, Call&& call)
        {
            m_guardError.clear();
            if (RejectIfOpen(capability, operation))
            {
                return false;
            }
            bool succeeded = false;
            InvokeAdapter(operation, [&] { succeeded = call(); });
            RecordOutcome(capability, succeeded);
            return succeeded;
        }

        template <typename Call>
        auto GuardQuery(const OnlineCapability capability, const char* operation, Call&& call) -> decltype(call())
        {
            m_guardError.clear();
            decltype(call()) result{};
            if (RejectIfOpen(capability, operation))
            {
                return result;
            }
            const bool completed = InvokeAdapter(operation, [&] { result = call(); });
            // An empty result is a legitimate answer (an empty board) unless the adapter gave a reason.
            const bool succeeded = completed && (!result.empty() || GetLastError().empty());
            RecordOutcome(capability, succeeded);
            if (!completed)
            {
                result = {};
            }
            return result;
        }

        template <typename Call> void GuardVoid(const char* operation, Call&& call)
        {
            m_guardError.clear();
            InvokeAdapter(operation, std::forward<Call>(call));
        }

        template <typename Result, typename Call>
        Result GuardConst(const char* operation, Result fallback, Call&& call) const
        {
            if (!m_target)
            {
                return fallback;
            }
            Result result = fallback;
            if (!InvokeAdapter(operation, [&] { result = call(); }))
            {
                return fallback;
            }
            return result;
        }

        // A throwing Login adapter may put the token in its exception text; never surface it.
        void RedactFromGuardError(const std::string& secret)
        {
            if (secret.empty())
            {
                return;
            }
            for (size_t pos = m_guardError.find(secret); pos != std::string::npos; pos = m_guardError.find(secret, pos))
            {
                m_guardError.replace(pos, secret.size(), "<redacted>");
            }
        }

        IOnlinePlatform* m_target = nullptr;
        bool m_circuitEnabled = false;
        OnlineCircuitPolicy m_policy;
        double m_nowSeconds = 0.0;
        std::array<OnlineCapabilityHealth, static_cast<size_t>(OnlineCapability::Count)> m_health{};
        // Failure raised by this front (exception or open circuit) for the most recent call;
        // mutable because const getters convert adapter exceptions too.
        mutable std::string m_guardError;
    };

    // ========================================================================
    // Manager singleton
    // ========================================================================

    /**
     * @brief Online service manager — singleton that holds the active platform
     *
     * Defaults to NullOnlinePlatform which is fully functional offline.
     * Call `SetPlatform()` to switch to Steam, Epic, or Console backends.
     * GetPlatform() returns the GuardedOnlinePlatform front, so every caller gets the
     * degraded-dependency semantics of docs/specs/online-services.md section 5.1.
     */
    class OnlineServiceManager
    {
      public:
        static OnlineServiceManager& GetInstance()
        {
            static OnlineServiceManager instance;
            return instance;
        }

        /** @brief Initialize with the default (Null) platform */
        void Initialize()
        {
            m_nullPlatform = std::make_unique<NullOnlinePlatform>();
            m_customPlatform.reset();
            m_activePlatform = m_nullPlatform.get();
            m_guard.Attach(m_activePlatform, false);
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "OnlineServiceManager initialized (Null platform)");
        }

        /** @brief Shut down and release all platforms */
        void Shutdown()
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "OnlineServiceManager shutting down");
            if (m_activePlatform)
                m_guard.Logout(); // Guarded: a throwing adapter cannot abort shutdown
            m_guard.Attach(nullptr, false);
            m_activePlatform = nullptr;
            m_customPlatform.reset();
            m_nullPlatform.reset();
            m_initialized = false;
        }

        /**
         * @brief Per-frame update: advances the clock that open circuits cool down on
         * @param deltaTime Frame time in seconds (non-positive or non-finite values are ignored)
         */
        void Update(float deltaTime) { m_guard.AdvanceTime(deltaTime); }

        /** @brief Get the active platform interface (the guarded front), or nullptr before Initialize() */
        IOnlinePlatform* GetPlatform() { return m_activePlatform ? &m_guard : nullptr; }

        /**
         * @brief Set a custom platform implementation
         * @param platform Unique pointer to the platform (manager takes ownership)
         */
        void SetPlatform(std::unique_ptr<IOnlinePlatform> platform)
        {
            if (!platform)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "OnlineServiceManager::SetPlatform called with null platform, reverting to Null");
                ResetToNullPlatform();
                return;
            }
            m_customPlatform = std::move(platform);
            m_activePlatform = m_customPlatform.get();
            m_guard.Attach(m_activePlatform, true);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Online platform changed to: %s",
                           m_guard.GetPlatformName().c_str());
        }

        /** @brief Reset to the default Null platform */
        void ResetToNullPlatform()
        {
            m_customPlatform.reset();
            m_activePlatform = m_nullPlatform.get();
            m_guard.Attach(m_activePlatform, false);
        }

        /** @brief Replace the circuit-breaker budget (defaults match the spec: 5 failures, 30 s) */
        void SetCircuitPolicy(const OnlineCircuitPolicy& policy) { m_guard.SetPolicy(policy); }

        /** @brief Failure accounting for one capability of the active adapter */
        const OnlineCapabilityHealth& GetCapabilityHealth(const OnlineCapability capability) const
        {
            return m_guard.GetHealth(capability);
        }

        /** @brief Get console-friendly status */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[OnlineServices] Not initialized";
            std::string status = "[OnlineServices] Platform: ";
            status += m_activePlatform ? m_guard.GetPlatformName() : "None";
            if (m_activePlatform)
            {
                const auto caps = m_guard.GetCapabilities();
                const bool hasAnyCapability = caps.authentication || caps.sessions || caps.leaderboards ||
                                              caps.achievements || caps.cloudSave || caps.friends || caps.presence;
                status += hasAnyCapability ? " | Capabilities: active" : " | Capabilities: none";
                const std::string error = m_guard.GetLastError();
                if (!error.empty())
                {
                    status += " | LastError: " + error;
                }
                status += " | Health: " + FormatHealth();
            }
            if (m_activePlatform && m_guard.IsLoggedIn())
            {
                auto player = m_guard.GetLocalPlayer();
                status += " | Player: " + player.displayName;
            }
            return status;
        }

      private:
        OnlineServiceManager() = default;

        // "ok", or one entry per capability that has failed since its last success.
        std::string FormatHealth() const
        {
            std::string health;
            for (size_t i = 0; i < static_cast<size_t>(OnlineCapability::Count); ++i)
            {
                const auto capability = static_cast<OnlineCapability>(i);
                const OnlineCapabilityHealth& entry = m_guard.GetHealth(capability);
                if (entry.consecutiveFailures == 0 && !entry.circuitOpen)
                {
                    continue;
                }
                health += health.empty() ? "" : ", ";
                health += std::format("{} {} consecutive failures", OnlineCapabilityName(capability),
                                      entry.consecutiveFailures);
                if (entry.circuitOpen)
                {
                    const double remaining = std::max(0.0, entry.retryAtSeconds - m_guard.GetNowSeconds());
                    health += remaining > 0.0 ? std::format(" (circuit open, retry in {:.1f}s)", remaining)
                                              : std::string(" (circuit open, probe allowed)");
                }
            }
            if (!m_guard.IsCircuitEnabled())
            {
                health +=
                    health.empty() ? "ok (circuit disabled: local adapter)" : " (circuit disabled: local adapter)";
            }
            return health.empty() ? "ok" : health;
        }

        bool m_initialized = false;
        IOnlinePlatform* m_activePlatform = nullptr;
        std::unique_ptr<NullOnlinePlatform> m_nullPlatform;
        std::unique_ptr<IOnlinePlatform> m_customPlatform;
        GuardedOnlinePlatform m_guard;
    };

} // namespace Spark::OnlineServices
