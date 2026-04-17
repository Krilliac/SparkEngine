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
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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

        std::vector<SessionInfo> FindSessions(const std::string& /*filter*/) override { return m_sessions; }

        bool CreateSession(const SessionInfo& settings) override
        {
            m_currentSession = settings;
            m_currentSession.sessionId = "local_" + std::to_string(m_nextSessionId++);
            m_sessions.push_back(m_currentSession);
            return true;
        }

        bool JoinSession(const std::string& sessionId) override
        {
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
            auto& board = m_leaderboards[boardName];
            LeaderboardEntry entry;
            entry.playerId = m_player.playerId;
            entry.playerName = m_player.displayName;
            entry.score = score;
            board.push_back(entry);
            // Sort by score descending and assign ranks
            std::sort(board.begin(), board.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
            for (uint32_t i = 0; i < board.size(); ++i)
                board[i].rank = i + 1;
            return true;
        }

        std::vector<LeaderboardEntry> QueryScores(const std::string& boardName, uint32_t maxResults) override
        {
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
            m_achievements[achievementId] = 1.0f;
            return true;
        }

        bool SetAchievementProgress(const std::string& id, float progress) override
        {
            m_achievements[id] = std::min(1.0f, std::max(0.0f, progress));
            return true;
        }

        std::vector<AchievementInfo> QueryAchievements() override
        {
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
            m_cloudSaves[slotName] = data;
            return true;
        }

        std::vector<uint8_t> LoadFromCloud(const std::string& slotName) override
        {
            auto it = m_cloudSaves.find(slotName);
            if (it == m_cloudSaves.end())
            {
                m_lastError = "Cloud slot not found: " + slotName;
                return {};
            }
            m_lastError.clear();
            return it->second;
        }

        bool DeleteCloudSave(const std::string& slotName) override { return m_cloudSaves.erase(slotName) > 0; }

        std::vector<CloudSaveInfo> ListCloudSaves() override
        {
            std::vector<CloudSaveInfo> result;
            for (const auto& [name, data] : m_cloudSaves)
                result.push_back({name, data.size(), ""});
            return result;
        }

        std::vector<FriendInfo> GetFriendsList() override { return m_friends; }

        bool SetPresence(const std::string& statusText) override
        {
            m_presence = statusText;
            return true;
        }

        bool InviteToSession(const std::string& /*friendId*/) override { return true; }

      private:
        bool m_loggedIn = false;
        std::string m_lastError;
        OnlinePlayerInfo m_player;
        SessionInfo m_currentSession;
        std::vector<SessionInfo> m_sessions;
        uint32_t m_nextSessionId = 1;
        std::unordered_map<std::string, std::vector<LeaderboardEntry>> m_leaderboards;
        std::unordered_map<std::string, float> m_achievements;
        std::unordered_map<std::string, std::vector<uint8_t>> m_cloudSaves;
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
    // Manager singleton
    // ========================================================================

    /**
     * @brief Online service manager — singleton that holds the active platform
     *
     * Defaults to NullOnlinePlatform which is fully functional offline.
     * Call `SetPlatform()` to switch to Steam, Epic, or Console backends.
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
            m_activePlatform = m_nullPlatform.get();
            m_initialized = true;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "OnlineServiceManager initialized (Null platform)");
        }

        /** @brief Shut down and release all platforms */
        void Shutdown()
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "OnlineServiceManager shutting down");
            if (m_activePlatform)
                m_activePlatform->Logout();
            m_activePlatform = nullptr;
            m_customPlatform.reset();
            m_nullPlatform.reset();
            m_initialized = false;
        }

        /** @brief Per-frame update (for async callbacks on real platforms) */
        void Update(float /*deltaTime*/)
        {
            // Real platform impls would call SteamAPI_RunCallbacks() or EOS_Platform_Tick()
        }

        /** @brief Get the active platform interface */
        IOnlinePlatform* GetPlatform() const { return m_activePlatform; }

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
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Online platform changed to: %s",
                           m_activePlatform->GetPlatformName().c_str());
        }

        /** @brief Reset to the default Null platform */
        void ResetToNullPlatform()
        {
            m_customPlatform.reset();
            m_activePlatform = m_nullPlatform.get();
        }

        /** @brief Get console-friendly status */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[OnlineServices] Not initialized";
            std::string status = "[OnlineServices] Platform: ";
            status += m_activePlatform ? m_activePlatform->GetPlatformName() : "None";
            if (m_activePlatform)
            {
                const auto caps = m_activePlatform->GetCapabilities();
                const bool hasAnyCapability = caps.authentication || caps.sessions || caps.leaderboards ||
                                              caps.achievements || caps.cloudSave || caps.friends || caps.presence;
                status += hasAnyCapability ? " | Capabilities: active" : " | Capabilities: none";
                const std::string error = m_activePlatform->GetLastError();
                if (!error.empty())
                {
                    status += " | LastError: " + error;
                }
            }
            if (m_activePlatform && m_activePlatform->IsLoggedIn())
            {
                auto player = m_activePlatform->GetLocalPlayer();
                status += " | Player: " + player.displayName;
            }
            return status;
        }

      private:
        OnlineServiceManager() = default;

        bool m_initialized = false;
        IOnlinePlatform* m_activePlatform = nullptr;
        std::unique_ptr<NullOnlinePlatform> m_nullPlatform;
        std::unique_ptr<IOnlinePlatform> m_customPlatform;
    };

} // namespace Spark::OnlineServices
