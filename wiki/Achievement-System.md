# Achievement System

SparkEngine provides a platform-agnostic achievement and statistics tracking system. It supports defining achievements with stat-based conditions, persistent statistics, and callbacks for bridging to platform APIs (Steam, Xbox, PlayStation).

**Source:** `SparkEngine/Source/Engine/Stats/AchievementSystem.h`

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

## Console Commands

```
achievement_status    # Show unlock progress
achievement_stats     # List all tracked statistics
```

---

## See Also

- [Save System](Save-System) — Persisting achievement data
- [Event System](Event-System) — Triggering stat updates from game events
- [UI System](UI-System) — Achievement unlock notifications
