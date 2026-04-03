# Achievement System

SparkEngine provides a platform-agnostic achievement and statistics tracking system. It supports defining achievements with stat-based conditions, persistent statistics, and callbacks for bridging to platform APIs (Steam, Xbox, PlayStation).

**Source:** `SparkEngine/Source/Engine/Gameplay/` (planned -- AchievementSystem is not yet implemented as a standalone header; achievement enums are in `SparkEngine/Source/Enums/GameSystemEnums.h`)

## Overview

| Class | Responsibility |
|-------|---------------|
| `AchievementSystem` | Singleton: defines achievements, tracks stats, fires unlock events |
| `Achievement` | Definition with ID, name, description, condition, progress |
| `Statistic` | A tracked integer or float value with aggregation type |

## Statistic Types

```cpp
enum class StatType {
    Integer, // Whole number (kills, deaths)
    Float,   // Fractional value (play time, distance)
    Maximum, // Tracks highest value seen (best score)
    Minimum  // Tracks lowest value seen (fastest time)
};
```

## Quick Start

```cpp
auto& achievements = AchievementSystem::Get();

// Define achievements and stats
achievements.DefineAchievement("first_blood", "First Blood", "Get your first kill");
achievements.DefineAchievement("sharpshooter", "Sharpshooter", "Get 100 headshots", true);
achievements.DefineStatistic("kills", StatType::Integer);
achievements.DefineStatistic("play_time", StatType::Float);

// Set unlock conditions
achievements.SetCondition("first_blood", "kills", 1);
achievements.SetCondition("sharpshooter", "headshots", 100);

// During gameplay:
achievements.IncrementStat("kills", 1);       // Auto-checks conditions
achievements.AddStatFloat("play_time", deltaTime);

// Manual unlock (for script-triggered achievements)
achievements.UnlockAchievement("story_complete");
```

## Platform Integration

Register a callback to bridge to platform achievement APIs:

```cpp
achievements.OnAchievementUnlocked([](const std::string& id) {
    SteamUserStats()->SetAchievement(id.c_str());
    SteamUserStats()->StoreStats();
    ShowUnlockNotification(id);
});
```

## Persistence

```cpp
achievements.SaveToFile("Data/Save/stats.json");
achievements.LoadFromFile("Data/Save/stats.json");
achievements.ResetAll();  // Reset all progress
```

## Querying

```cpp
const Achievement* ach = achievements.GetAchievement("first_blood");
auto all = achievements.GetAllAchievements();
size_t unlocked = achievements.GetUnlockedCount();
size_t total = achievements.GetTotalCount();

int64_t kills = achievements.GetStatInt("kills");
double playTime = achievements.GetStatFloat("play_time");
```

## Thread Safety

`AchievementSystem` is thread-safe (mutex-protected singleton).

## Achievement Categories

Achievements are organized into categories for display in the achievement browser UI. Each achievement belongs to exactly one category, and categories control grouping, sorting, and filter tabs.

```cpp
enum class AchievementCategory {
    Story,        // Main campaign progression
    Combat,       // Kill-based and weapon challenges
    Exploration,  // Discovering areas, collectibles
    Challenge,    // Skill-based (no-death runs, speed runs)
    Secret,       // Hidden achievements (description hidden until unlocked)
    Multiplayer   // Online-specific achievements
};

achievements.DefineAchievement("first_blood", "First Blood", "Get your first kill",
    AchievementCategory::Combat);
achievements.DefineAchievement("find_bunker", "Hidden Bunker", "???",
    AchievementCategory::Secret, /*hidden=*/true);
```

Hidden (secret) achievements display a placeholder description like "???" in the UI until they are unlocked. This prevents spoilers for story and exploration achievements.

### Querying by Category

```cpp
auto combatAchievements = achievements.GetByCategory(AchievementCategory::Combat);
size_t combatUnlocked = achievements.GetUnlockedCount(AchievementCategory::Combat);
size_t combatTotal = achievements.GetTotalCount(AchievementCategory::Combat);
float combatPercent = achievements.GetCompletionPercent(AchievementCategory::Combat);
```

## Progressive Achievement Tracking with Percentage Display

Many achievements require accumulating progress over time (e.g., "Get 1000 kills"). The achievement system tracks and exposes progress as a normalized percentage for UI display.

```cpp
// Define a progressive achievement
achievements.DefineAchievement("veteran", "Veteran", "Get 1000 kills",
    AchievementCategory::Combat, /*progressive=*/true);
achievements.SetCondition("veteran", "kills", 1000);

// Query progress
const Achievement* ach = achievements.GetAchievement("veteran");
float progress = ach->GetProgress();           // 0.0 to 1.0
int64_t current = ach->GetCurrentValue();      // e.g., 347
int64_t target = ach->GetTargetValue();        // e.g., 1000
std::string display = ach->GetProgressText();  // "347 / 1000"
```

The UI can display progressive achievements with a progress bar and percentage text. Non-progressive achievements show only locked/unlocked state.

### Multi-Condition Achievements

Some achievements require multiple conditions to be met simultaneously:

```cpp
// "Renaissance Man" — get 50 kills with 5 different weapon types
achievements.DefineAchievement("renaissance", "Renaissance Man",
    "Get 50 kills with 5 different weapon types", AchievementCategory::Challenge);
achievements.SetMultiCondition("renaissance", {
    {"rifle_kills", 50},
    {"pistol_kills", 50},
    {"shotgun_kills", 50},
    {"sniper_kills", 50},
    {"melee_kills", 50}
}, MultiConditionMode::All);  // All conditions must be met
```

## Notification System Integration

When an achievement is unlocked, the system fires a notification through SparkEngine's event system. The default notification displays a toast popup with the achievement icon, name, and description.

```cpp
// Configure notification appearance
achievements.SetNotificationDuration(5.0f);        // Display for 5 seconds
achievements.SetNotificationPosition(NotifyPos::TopRight);
achievements.SetNotificationSound("Data/Audio/UI/achievement_unlock.wav");

// Custom notification rendering callback
achievements.OnNotificationRender([](const Achievement& ach, float alpha) {
    DrawAchievementToast(ach.GetIcon(), ach.GetName(), ach.GetDescription(), alpha);
});

// Progress milestone notifications (fires at 25%, 50%, 75%)
achievements.EnableProgressMilestones(true);
achievements.SetMilestoneIntervals({0.25f, 0.50f, 0.75f});
```

Progress milestone notifications display messages like "Veteran: 75% complete (750/1000)" to keep the player informed of progress toward long-running achievements.

## Steam/Xbox/PlayStation API Bridging Patterns

The achievement system is platform-agnostic at its core but provides a callback-based bridging pattern for integrating with platform-specific achievement APIs.

### Steam Integration

```cpp
achievements.OnAchievementUnlocked([](const std::string& id) {
    SteamUserStats()->SetAchievement(id.c_str());
    SteamUserStats()->StoreStats();
});

achievements.OnStatUpdated([](const std::string& statName, int64_t value) {
    SteamUserStats()->SetStat(statName.c_str(), static_cast<int32>(value));
});

// Sync from Steam on startup (handles achievements unlocked on other devices)
void SyncFromSteam(AchievementSystem& achievements)
{
    for (const auto& ach : achievements.GetAllAchievements())
    {
        bool unlocked = false;
        SteamUserStats()->GetAchievement(ach.GetId().c_str(), &unlocked);
        if (unlocked && !ach.IsUnlocked())
            achievements.UnlockAchievement(ach.GetId(), /*silent=*/true);
    }
}
```

### Xbox GDK Integration

```cpp
achievements.OnAchievementUnlocked([](const std::string& id) {
    XblAchievementsUpdateAchievementAsync(
        xboxLiveContext, xuid, id.c_str(), 100, // 100% progress
        nullptr, nullptr);
});
```

### PlayStation Integration

```cpp
achievements.OnAchievementUnlocked([](const std::string& id) {
    SceNpTrophyId trophyId = MapToTrophyId(id);
    sceNpTrophyUnlockTrophy(trophyContext, trophyHandle, trophyId, &platinumId);
});
```

Each platform bridge is implemented as a separate module that registers callbacks at initialization. The core `AchievementSystem` has no platform-specific dependencies.

## Achievement Art and Icons

Each achievement can have an associated icon displayed in the achievement browser and unlock notifications.

```cpp
achievements.SetIcon("first_blood", "Data/Textures/Achievements/first_blood.png");
achievements.SetIcon("veteran", "Data/Textures/Achievements/veteran.png");

// Locked achievements display a generic locked icon
achievements.SetLockedIcon("Data/Textures/Achievements/locked.png");

// Icons are loaded on demand and cached in a texture atlas
// Recommended icon size: 128x128 pixels, PNG format with transparency
```

## Leaderboard Integration

The achievement system includes optional leaderboard support for tracking competitive statistics.

```cpp
// Define a leaderboard
achievements.DefineLeaderboard("fastest_completion", "Fastest Level Completion",
    LeaderboardSort::Ascending, LeaderboardFormat::TimeMilliseconds);

// Submit a score
achievements.SubmitLeaderboardScore("fastest_completion", completionTimeMs);

// Query leaderboard (async, results via callback)
achievements.QueryLeaderboard("fastest_completion", LeaderboardRange::AroundPlayer, 10,
    [](const std::vector<LeaderboardEntry>& entries) {
        for (const auto& entry : entries)
            DisplayLeaderboardRow(entry.rank, entry.playerName, entry.score);
    });
```

Leaderboard data is stored server-side in live-service configurations, or locally in the save file for offline play.

## Cloud Save for Achievement Progress

Achievement and statistic data can be synchronized with cloud storage to persist progress across devices.

```cpp
// Save to cloud (async)
achievements.SaveToCloud("player_achievements", [](bool success) {
    if (!success)
        LogWarning("Failed to sync achievements to cloud");
});

// Load from cloud on startup
achievements.LoadFromCloud("player_achievements", [](bool success) {
    if (success)
        LogInfo("Achievement progress restored from cloud");
    else
        achievements.LoadFromFile("Data/Save/stats.json");  // Fallback to local
});
```

Cloud save handles conflict resolution when local and cloud data diverge. The default policy is "highest progress wins" — for each statistic, the higher value is kept, and any achievement unlocked on either side remains unlocked.

## Per-Session vs Persistent Statistics

Statistics are tracked in two scopes: session (reset each game session) and persistent (saved across sessions).

```cpp
// Persistent stats (saved to disk)
achievements.DefineStatistic("total_kills", StatType::Integer, StatScope::Persistent);
achievements.DefineStatistic("total_play_time", StatType::Float, StatScope::Persistent);

// Session stats (reset on game restart)
achievements.DefineStatistic("session_kills", StatType::Integer, StatScope::Session);
achievements.DefineStatistic("session_deaths", StatType::Integer, StatScope::Session);
achievements.DefineStatistic("session_kd_ratio", StatType::Float, StatScope::Session);

// Session stats are useful for UI displays ("This session: 47 kills, 12 deaths")
// Persistent stats drive achievement conditions
```

Session statistics are computed from persistent data where possible. For example, K/D ratio for the current session is derived from session kills and session deaths.

## Time-Based Statistics Tracking

The system supports automatic time-based tracking for statistics like play time, time alive, and time spent in specific game modes.

```cpp
// Auto-tracked timers (increment automatically while conditions are met)
achievements.DefineTimer("play_time", "Total play time");
achievements.DefineTimer("time_alive", "Time alive without dying");
achievements.DefineTimer("time_in_vehicle", "Time spent in vehicles");

// Start/stop timers based on game state
achievements.StartTimer("play_time");          // Starts at game launch
achievements.StartTimer("time_alive");          // Starts at spawn

// On death:
achievements.StopTimer("time_alive");
float survivalTime = achievements.GetTimerValue("time_alive");
achievements.ResetTimer("time_alive");          // Reset for next life

// Timers integrate with achievements
achievements.SetCondition("marathon", "play_time", 36000.0f);  // 10 hours
```

## Bulk Operations for Achievement Management

For games with large numbers of achievements, bulk operations are provided for efficient management.

```cpp
// Bulk define from JSON configuration
achievements.LoadDefinitionsFromFile("Data/Config/achievements.json");

// The JSON format:
// {
//   "achievements": [
//     {
//       "id": "first_blood",
//       "name": "First Blood",
//       "description": "Get your first kill",
//       "category": "combat",
//       "icon": "Data/Textures/Achievements/first_blood.png",
//       "condition": { "stat": "kills", "target": 1 }
//     },
//     ...
//   ],
//   "statistics": [
//     { "id": "kills", "type": "integer", "scope": "persistent" },
//     ...
//   ]
// }

// Bulk reset by category
achievements.ResetCategory(AchievementCategory::Multiplayer);

// Bulk lock/unlock (for save file migration)
achievements.LockAll();
achievements.UnlockMultiple({"first_blood", "story_complete", "veteran"});

// Export achievement report (useful for analytics)
std::string report = achievements.GenerateReport();
// Includes: total progress, per-category breakdown, rarest achievements, stat summaries
```

## Debug Commands for Testing Achievements

During development, console commands allow rapid testing of achievement logic without playing through the entire game.

```cpp
// Console commands available in debug builds only
```

## Console Commands

```
achievement_status            # Show unlock progress
achievement_stats             # List all tracked statistics
achievement_unlock <id>       # Force-unlock an achievement (debug only)
achievement_lock <id>         # Re-lock an achievement (debug only)
achievement_unlock_all        # Unlock every achievement (debug only)
achievement_lock_all          # Lock every achievement (debug only)
achievement_set_stat <id> <v> # Set a statistic to a specific value (debug only)
achievement_reset             # Reset all achievements and statistics
achievement_progress <id>     # Show detailed progress for one achievement
achievement_list [category]   # List achievements, optionally filtered by category
achievement_export <file>     # Export achievement state to JSON file
achievement_import <file>     # Import achievement state from JSON file
achievement_notify_test <id>  # Trigger a test notification for an achievement
```

Debug commands are stripped from release builds via conditional compilation. They are invaluable for QA testing, allowing testers to quickly verify unlock conditions, notification rendering, and platform API bridging without replaying content.

---

## See Also

- [Save System](Save-System) — Persisting achievement data
- [Event System](Event-System) — Triggering stat updates from game events
- [UI System](UI-System) — Achievement unlock notifications
