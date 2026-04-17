# Timer Manager

Centralized gameplay timer service with named timers supporting one-shot and looping modes, pause/resume, rate control, and fire count tracking.

**Source:** `SparkEngine/Source/Utils/TimerManager.h`

## Overview

The Timer Manager provides a structured alternative to ad-hoc cooldown tracking in gameplay code. Inspired by Unreal Engine's `FTimerManager`, it maintains a pool of named timers that tick each frame. Timers fire a callback when their interval elapses, and can be configured as one-shot (auto-removed after firing) or looping (repeats until explicitly cleared).

Each timer has a human-readable name used for lookup, which makes debugging straightforward. Timers can be paused and resumed without losing elapsed progress, and their rate (interval) can be changed at runtime. The system tracks how many times each timer has fired, useful for gameplay mechanics like damage-over-time stacks or periodic spawning.

The update loop processes pending removals first (to safely handle timers cleared during callbacks), then advances all active timers. Looping timers subtract the rate from elapsed time rather than resetting to zero, which correctly handles cases where `deltaTime` exceeds the timer interval. One-shot timers are automatically queued for removal after firing.

## Key Classes

| Class / Struct | Description |
|---|---|
| `TimerManager` | Singleton that owns and ticks all named timers |
| `ManagedTimer` | A single timer: name, rate, elapsed, looping flag, state, callback, fire count |
| `TimerState` | Enum: `Active` (counting down), `Paused` (frozen), `Expired` (pending removal) |
| `TimerCallback` | `std::function<void()>` called when the timer fires |

## Usage

```cpp
auto& timers = Spark::TimerManager::GetInstance();
timers.Initialize();

// One-shot timer: respawn player after 3 seconds
timers.SetTimer("respawn", 3.0f, false, []() {
    SpawnPlayer();
});

// Looping timer: regenerate health every 1 second
timers.SetTimer("regen", 1.0f, true, [&player]() {
    player.health = std::min(player.health + 5, player.maxHealth);
});

// Looping timer: spawn enemies every 10 seconds
timers.SetTimer("enemy_wave", 10.0f, true, []() {
    SpawnEnemyWave();
});

// Pause regen during combat
timers.PauseTimer("regen");

// Resume when combat ends
timers.ResumeTimer("regen");

// Speed up enemy spawns mid-game
timers.SetTimerRate("enemy_wave", 5.0f);

// Query timer state
float remaining = timers.GetRemainingTime("respawn");
bool active = timers.IsTimerActive("respawn");
uint32_t waves = timers.GetFireCount("enemy_wave");

// Cancel a timer
timers.ClearTimer("regen");

// In the main loop
void MainLoop(float deltaTime)
{
    timers.Update(deltaTime);
    // ...
}
```

## API Reference

### Lifecycle

| Method | Return | Description |
|---|---|---|
| `Initialize()` | `void` | Initialize the timer manager and clear all timers |
| `Shutdown()` | `void` | Clear all timers and shut down |
| `Update(deltaTime)` | `void` | Tick all active timers; call once per frame |

### Timer Management

| Method | Return | Description |
|---|---|---|
| `SetTimer(name, rate, looping, callback)` | `void` | Create or replace a named timer |
| `ClearTimer(name)` | `void` | Remove a timer (deferred to next Update) |
| `ClearAllTimers()` | `void` | Remove all timers immediately |
| `PauseTimer(name)` | `void` | Pause a timer, preserving elapsed time |
| `ResumeTimer(name)` | `void` | Resume a paused timer |
| `SetTimerRate(name, newRate)` | `void` | Change the interval of an existing timer |
| `ResetTimer(name)` | `void` | Reset elapsed time to zero and set state to Active |

### Query

| Method | Return | Description |
|---|---|---|
| `IsTimerActive(name)` | `bool` | Whether the timer exists and is actively counting |
| `TimerExists(name)` | `bool` | Whether the timer exists in any state |
| `IsTimerPaused(name)` | `bool` | Whether the timer is paused |
| `GetRemainingTime(name)` | `float` | Seconds until next fire |
| `GetElapsedTime(name)` | `float` | Seconds since last fire (or since creation) |
| `GetFireCount(name)` | `uint32_t` | Number of times this timer has fired |
| `GetTimerCount()` | `size_t` | Total number of managed timers |
| `GetTimerNames()` | `vector<string>` | Names of all timers |

## Related Systems

- [Tween System](../subsystems/Tween-System.md) -- value interpolation over time (complementary to timers)
- [Coroutine Scheduler](../subsystems/Coroutine-System.md) -- async frame-based scheduling
- [ECS Systems](../subsystems/Entity-Component-System.md) -- gameplay systems can use TimerManager for periodic logic
- [Event System](../subsystems/Event-System.md) -- timers can fire events instead of direct callbacks
