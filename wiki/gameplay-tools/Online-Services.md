# Online Services

Unified online platform integration with pluggable backends for authentication, matchmaking, leaderboards, achievements, cloud saves, and friends/presence.

**Source:** `SparkEngine/Source/Engine/OnlineServices/OnlineServices.h`

## Overview

The Online Services system provides a single abstract interface (`IOnlinePlatform`) that encapsulates all platform-specific online functionality. Games code against this interface and never reference Steam, Epic, or console APIs directly. At runtime, the `OnlineServiceManager` singleton holds one active platform implementation.

The default platform is `NullOnlinePlatform`, which is fully functional offline -- it stores leaderboards in memory, achievements as a set, and cloud saves to local storage. This means single-player games work out of the box without any SDK dependencies, and developers can test online flows without network connectivity.

Adding a new platform requires implementing the `IOnlinePlatform` interface and passing it to `OnlineServiceManager::SetPlatform()`. Stub classes for Steam, Epic, and Console are included with detailed integration guides in their documentation comments.

## Architecture

```
OnlineServiceManager (singleton)
  +-- IOnlinePlatform* (active platform pointer)
        |-- NullOnlinePlatform   (default -- fully functional offline)
        |-- SteamPlatform        (stub -- requires Steamworks SDK)
        |-- EpicPlatform         (stub -- requires EOS SDK)
        +-- ConsolePlatform      (stub -- requires NDA + dev kits)
```

## Key Classes

| Class | Description |
|-------|-------------|
| `IOnlinePlatform` | Abstract interface defining all online service methods |
| `NullOnlinePlatform` | Fully functional offline implementation (default) |
| `SteamPlatform` | Stub for Steamworks SDK integration |
| `EpicPlatform` | Stub for Epic Online Services (EOS) SDK integration |
| `ConsolePlatform` | Stub for PlayStation/Xbox/Switch (NDA-protected SDKs) |
| `OnlineServiceManager` | Singleton that owns and routes to the active platform |

## Data Structures

| Struct | Description |
|--------|-------------|
| `OnlinePlayerInfo` | Player ID, display name, online status |
| `SessionInfo` | Multiplayer session metadata (host, map, mode, player count) |
| `LeaderboardEntry` | Player score with rank |
| `AchievementInfo` | Achievement ID, progress, unlock state |
| `CloudSaveInfo` | Save slot name, size, timestamp |
| `FriendInfo` | Friend ID, display name, online status, presence text |

## Usage

```cpp
// Initialize with default offline platform
auto& online = Spark::OnlineServices::OnlineServiceManager::GetInstance();
online.Initialize();

// Use the platform interface
auto* platform = online.GetPlatform();
platform->Login("player1", "");
platform->SubmitScore("HighScores", 9999);
platform->UnlockAchievement("first_kill");
platform->SaveToCloud("save1", saveData);

// Switch to a custom platform at runtime
online.SetPlatform(std::make_unique<MySteamPlatform>());
```

### Adding a New Platform

```cpp
class MySteamPlatform : public Spark::OnlineServices::IOnlinePlatform
{
public:
    std::string GetPlatformName() const override { return "Steam"; }
    bool Login(const std::string& username, const std::string& token) override
    {
        // Initialize Steamworks SDK, authenticate user
        return SteamAPI_Init();
    }
    // ... implement all IOnlinePlatform methods
};
```

## API Reference

### OnlineServiceManager

| Method | Description |
|--------|-------------|
| `GetInstance()` | Get the singleton instance |
| `Initialize()` | Initialize with NullOnlinePlatform |
| `Shutdown()` | Log out and release all platforms |
| `Update(float dt)` | Per-frame update for async callbacks |
| `GetPlatform()` | Get the active `IOnlinePlatform*` |
| `SetPlatform(unique_ptr)` | Switch to a custom platform (takes ownership) |
| `ResetToNullPlatform()` | Revert to the offline platform |

### IOnlinePlatform

| Method | Description |
|--------|-------------|
| `Login() / Logout()` | Authentication |
| `FindSessions() / CreateSession() / JoinSession()` | Matchmaking |
| `SubmitScore() / QueryScores()` | Leaderboards |
| `UnlockAchievement() / SetAchievementProgress()` | Achievements |
| `SaveToCloud() / LoadFromCloud()` | Cloud saves |
| `GetFriendsList() / SetPresence() / InviteToSession()` | Social features |

## Configuration

| Setting | Description |
|---------|-------------|
| `-DENABLE_STEAM=ON` | Enable Steam SDK (requires `ThirdParty/Steamworks/`) |
| `-DENABLE_EOS=ON` | Enable Epic Online Services SDK (requires `ThirdParty/EOS/`) |

## Related Systems

- [Networking](Networking-System) -- UDP transport for gameplay networking
- [Save System](Save-System.md) -- Local save/load persistence
- [Gameplay Systems](Gameplay-Systems.md) -- Inventory, quests, achievements
