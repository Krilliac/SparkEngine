# Tween System

SparkEngine provides a handle-based tween system for smoothly interpolating values over time. The system supports sequencing, parallel composition, configurable easing, looping, and lifecycle control — suitable for UI animations, camera movements, gameplay feedback, and any time-driven interpolation.

**Source:** `SparkEngine/Source/Engine/Tween/TweenSystem.h`
**Namespace:** `Spark`
**Tests:** `Tests/TestTween.cpp` (14 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Easing Functions](#easing-functions)
- [TweenInstance](#tweeninstance)
  - [Chaining API](#chaining-api)
  - [Loop Modes](#loop-modes)
  - [Lifecycle Control](#lifecycle-control)
- [TweenHandle](#tweenhandle)
- [TweenSystem](#tweensystem)
  - [Singleton Access](#singleton-access)
  - [Creating Tweens](#creating-tweens)
  - [Convenience Helpers](#convenience-helpers)
  - [Composition](#composition)
  - [Update Loop](#update-loop)
- [Practical Examples](#practical-examples)
  - [Fade In UI Element](#fade-in-ui-element)
  - [Camera Dolly](#camera-dolly)
  - [Sequenced Door Animation](#sequenced-door-animation)
  - [Looping Pulse Effect](#looping-pulse-effect)
- [Relationship to Utils/Tween.h](#relationship-to-utilstweenh)
- [Integration](#integration)
- [See Also](#see-also)

---

## Overview

The tween system solves the problem of time-based value interpolation without scattering timer logic across update functions. Instead of manually tracking elapsed time and computing lerp values, you describe the interpolation once and let the system drive it.

```
┌─────────────────────────────────────────────────────────────┐
│                       Game Code                             │
│   TweenFloat(), CreateSequence(), CreateParallel()          │
├─────────────────────────────────────────────────────────────┤
│                      TweenSystem                            │
│  ┌──────────────┬──────────────────┬──────────────────────┐ │
│  │ Handle       │ Update           │ Composition          │ │
│  │ Registry     │ Loop             │                      │ │
│  │              │                  │                      │ │
│  │ Create()     │ Update(dt)       │ CreateSequence()     │ │
│  │ Get()        │ (drives all      │ CreateParallel()     │ │
│  │ Cancel()     │  active tweens)  │                      │ │
│  └──────────────┴──────────────────┴──────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│  TweenInstance          TweenHandle         EaseType        │
│  (value, callbacks,     (uint32_t ID)       (10 easing      │
│   easing, loop)                              functions)      │
└─────────────────────────────────────────────────────────────┘
```

### Key Types

| Type | Responsibility |
|------|---------------|
| `TweenSystem` | Singleton manager: creates, updates, and destroys tweens by handle |
| `TweenInstance` | A single interpolation with duration, easing, callbacks, and loop configuration |
| `TweenHandle` | Lightweight `uint32_t` identifier for referencing a tween |
| `EaseType` | Enum of 10 easing functions controlling the interpolation curve |
| `TweenState` | Enum tracking tween lifecycle: Pending, Running, Paused, Completed |
| `LoopMode` | Enum for loop behavior: None, Loop, PingPong |

---

## Easing Functions

The `EaseType` enum provides 10 built-in easing curves. The `ApplyEase()` free function maps a linear `t` in `[0, 1]` to the eased value.

```cpp
float ApplyEase(EaseType ease, float t);
```

| EaseType | Behavior | Best For |
|----------|----------|----------|
| `Linear` | Constant speed | Mechanical motion, progress bars |
| `EaseInQuad` | Slow start, accelerate | Objects starting from rest |
| `EaseOutQuad` | Fast start, decelerate | Objects coming to rest |
| `EaseInOutQuad` | Slow start and end | Smooth camera transitions |
| `EaseInCubic` | Slower start than quad | Dramatic acceleration |
| `EaseOutCubic` | Slower end than quad | Soft landing effects |
| `EaseInOutCubic` | Smooth S-curve | Polished UI animations |
| `EaseInElastic` | Elastic wind-up | Anticipation effects |
| `EaseOutElastic` | Elastic overshoot | Bouncy UI elements |
| `EaseOutBounce` | Bouncing at end | Physical bounce effects |

---

## TweenInstance

A `TweenInstance` represents a single interpolation operation. It is created by the `TweenSystem` and configured via a chaining API.

```cpp
class TweenInstance
{
public:
    using UpdateCallback = std::function<void(float t)>;
    using CompleteCallback = std::function<void()>;

    TweenInstance(float duration, UpdateCallback onUpdate,
                  EaseType ease = EaseType::Linear);

    TweenInstance& OnComplete(CompleteCallback callback);
    TweenInstance& SetLoop(LoopMode mode, int32_t count = -1);
    TweenInstance& SetDelay(float delay);
    TweenInstance& SetPlayRate(float rate);

    void Pause();
    void Resume();
    void Cancel();
    void Restart();

    TweenState GetState() const;
    float GetProgress() const;   // elapsed / duration
    float GetElapsed() const;
};
```

### Chaining API

After creating a tween, configure it with method chaining:

```cpp
auto handle = tweenSystem.Create(1.0f, [&](float t)
{
    opacity = t;  // t is the eased value in [0, 1]
}, EaseType::EaseOutQuad);

if (auto* tween = tweenSystem.Get(handle))
{
    tween->OnComplete([]() { LOG_INFO("Fade complete"); })
          .SetDelay(0.5f)
          .SetPlayRate(2.0f);
}
```

### Loop Modes

| LoopMode | Behavior |
|----------|----------|
| `None` | Play once, then complete |
| `Loop` | Restart from the beginning after each cycle |
| `PingPong` | Alternate forward and backward each cycle |

The `count` parameter controls how many times to loop. Pass `-1` for infinite looping.

```cpp
tween->SetLoop(LoopMode::PingPong, 3);  // 3 cycles: forward, back, forward
tween->SetLoop(LoopMode::Loop, -1);     // infinite loop
```

### Lifecycle Control

| Method | Effect |
|--------|--------|
| `Pause()` | Freezes the tween at its current progress |
| `Resume()` | Continues from where it was paused |
| `Cancel()` | Stops and marks as Completed (no callback) |
| `Restart()` | Resets elapsed time and resumes from the beginning |

---

## TweenHandle

```cpp
using TweenHandle = uint32_t;
constexpr TweenHandle INVALID_TWEEN = 0;
```

Handles are monotonically increasing IDs starting at 1. Use `INVALID_TWEEN` to check for invalid or uninitialized handles.

---

## TweenSystem

### Singleton Access

```cpp
auto& ts = TweenSystem::GetInstance();
ts.Initialize();
```

### Creating Tweens

```cpp
// Low-level: create with custom update callback
TweenHandle Create(float duration, TweenInstance::UpdateCallback onUpdate,
                   EaseType ease = EaseType::Linear);

// Get the instance for further configuration
TweenInstance* Get(TweenHandle handle);
```

### Convenience Helpers

```cpp
// Interpolate a float reference from start to end
TweenHandle TweenFloat(float& target, float from, float to,
                       float duration, EaseType ease = EaseType::Linear);

// Fire a callback after a delay (no interpolation)
TweenHandle TweenDelay(float delay, TweenInstance::CompleteCallback onComplete);
```

### Composition

Tweens can be composed into sequences (one after another) or parallel groups (all at once):

```cpp
// Play tweens one after another
TweenHandle CreateSequence(std::vector<TweenHandle> tweens);

// Play all tweens simultaneously, complete when the longest finishes
TweenHandle CreateParallel(std::vector<TweenHandle> tweens);
```

### Update Loop

Call `Update()` once per frame from the main game loop:

```cpp
ts.Update(deltaTime);
```

Completed tweens are automatically removed after their completion callback fires.

| Method | Description |
|--------|-------------|
| `Cancel(handle)` | Cancel a specific tween |
| `CancelAll()` | Cancel all active tweens |
| `Pause(handle)` | Pause a specific tween |
| `Resume(handle)` | Resume a paused tween |
| `GetActiveTweenCount()` | Number of tweens currently active |
| `Console_GetStatus()` | Formatted status string for debug console |

---

## Practical Examples

### Fade In UI Element

```cpp
auto& ts = TweenSystem::GetInstance();

float panelOpacity = 0.0f;
auto handle = ts.TweenFloat(panelOpacity, 0.0f, 1.0f, 0.3f,
                            EaseType::EaseOutQuad);
```

### Camera Dolly

```cpp
auto& ts = TweenSystem::GetInstance();

float cameraZ = 0.0f;
auto dolly = ts.TweenFloat(cameraZ, 0.0f, 10.0f, 2.0f,
                           EaseType::EaseInOutCubic);

if (auto* tween = ts.Get(dolly))
{
    tween->OnComplete([]()
    {
        LOG_INFO("Camera dolly complete");
    });
}
```

### Sequenced Door Animation

```cpp
auto& ts = TweenSystem::GetInstance();

float doorAngle = 0.0f;
float lightIntensity = 0.0f;

// Step 1: open the door
auto openDoor = ts.TweenFloat(doorAngle, 0.0f, 90.0f, 1.0f,
                              EaseType::EaseOutCubic);

// Step 2: wait 0.5s
auto pause = ts.TweenDelay(0.5f, nullptr);

// Step 3: fade in the light
auto fadeLight = ts.TweenFloat(lightIntensity, 0.0f, 1.0f, 0.5f,
                               EaseType::EaseInQuad);

// Play in order
auto sequence = ts.CreateSequence({openDoor, pause, fadeLight});
```

### Looping Pulse Effect

```cpp
auto& ts = TweenSystem::GetInstance();

float scale = 1.0f;
auto pulse = ts.Create(0.8f, [&](float t)
{
    scale = 1.0f + 0.1f * t;
}, EaseType::EaseInOutQuad);

if (auto* tween = ts.Get(pulse))
{
    tween->SetLoop(LoopMode::PingPong, -1);  // infinite pulse
}
```

---

## Relationship to Utils/Tween.h

SparkEngine has two tween systems:

| Feature | `Utils/Tween.h` | `Engine/Tween/TweenSystem.h` |
|---------|-----------------|------------------------------|
| Complexity | Simple, standalone | Full-featured singleton |
| Handle-based | No | Yes |
| Sequencing | No | Yes (`CreateSequence`) |
| Parallel groups | No | Yes (`CreateParallel`) |
| Loop modes | No | Yes (Loop, PingPong) |
| Delay / play rate | No | Yes |
| Best for | Quick one-off lerps | Production animation systems |

The `TweenSystem` in `Engine/Tween/` is the production-ready API. `Utils/Tween.h` is a lightweight helper for simple cases where handle management is unnecessary.

---

## Integration

- **Main loop**: `TweenSystem::GetInstance().Update(deltaTime)` must be called each frame
- **Initialization**: Call `TweenSystem::GetInstance().Initialize()` at engine startup
- **Shutdown**: Call `TweenSystem::GetInstance().Shutdown()` at engine teardown
- **Console**: `Console_GetStatus()` provides debug output for the [SparkConsole](../gameplay-tools/SparkConsole.md)

---

## See Also

- [Coroutine System](Coroutine-System.md) — For complex async gameplay sequences with branching logic
- [Animation](Animation.md) — For skeletal animation and state machines
- [UI System](UI-System.md) — Common use case for tween-driven transitions
- [Event System](Event-System.md) — Trigger tweens in response to game events
