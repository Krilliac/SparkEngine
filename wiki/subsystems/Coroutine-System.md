# Coroutine System

SparkEngine provides a cooperative coroutine scheduler for gameplay code that needs delayed, yielding, or repeating tasks. The system offers two complementary APIs: a **builder-pattern API** for simple sequenced actions and a **C++20 coroutine API** for natural async-style control flow using `co_await`, `co_yield`, and `co_return`.

All coroutines run cooperatively on the **main thread**. They yield control back to the scheduler each frame and are resumed when their yield condition is satisfied.

**Source:** `SparkEngine/Source/Engine/Coroutine/CoroutineScheduler.h`
**Namespace:** `Spark`
**Tests:** `Tests/TestCoroutineScheduler.cpp` (10 test cases)

---

## Table of Contents

- [Overview](#overview)
- [Yield Instructions](#yield-instructions)
  - [WaitForSeconds](#waitforseconds)
  - [WaitForFrames](#waitforframes)
  - [WaitUntil](#waituntil)
  - [WaitForEndOfFrame](#waitforendofframe)
  - [WaitForEvent](#waitforevent)
- [Builder-Pattern API](#builder-pattern-api)
  - [Coroutine Class](#coroutine-class)
  - [Builder Methods](#builder-methods)
  - [Builder Examples](#builder-examples)
- [C++20 Coroutine API](#c20-coroutine-api)
  - [GameCoroutine Return Type](#gamecoroutine-return-type)
  - [Promise Type](#promise-type)
  - [Supported co_await Expressions](#supported-co_await-expressions)
  - [C++20 Examples](#c20-examples)
- [CoroutineScheduler](#coroutinescheduler)
  - [Singleton Access](#singleton-access)
  - [Scheduler Methods](#scheduler-methods)
  - [Update Loop Integration](#update-loop-integration)
- [NativeCoroutineWrapper](#nativecoroutinewrapper)
- [Convenience Free Functions](#convenience-free-functions)
- [Integration with EventBus](#integration-with-eventbus)
- [Practical Examples](#practical-examples)
  - [Damage Flash HUD Effect](#damage-flash-hud-effect)
  - [Enemy Wave Spawner](#enemy-wave-spawner)
  - [Door Open Sequence](#door-open-sequence)
  - [Countdown Timer](#countdown-timer)
  - [Waiting for Player Input](#waiting-for-player-input)
- [Threading Model](#threading-model)
- [Testing](#testing)
- [See Also](#see-also)

---

## Overview

The coroutine system solves a common gameplay programming problem: expressing sequences of actions separated by time delays, frame waits, or condition checks without blocking the game loop. Instead of scattering state machines across update functions, you describe the full sequence in one place and let the scheduler handle timing.

The two APIs serve different use cases:

| Feature | Builder Pattern | C++20 Coroutines |
|---------|----------------|-----------------|
| Syntax | Fluent method chaining | `co_await` / `co_yield` / `co_return` |
| Control flow | Linear sequences only | Loops, branches, try/catch |
| Local variables | Captured by lambda | Naturally preserved across yields |
| Compiler support | Any C++17+ compiler | Requires C++20 coroutine support |
| Best for | Simple action-delay chains | Complex async logic |

Both APIs share the same `YieldInstruction` types and are managed by the same `CoroutineScheduler`.

---

## Yield Instructions

All yield instructions derive from the `YieldInstruction` base class, which defines the interface the scheduler uses to check whether a coroutine should resume.

```cpp
class YieldInstruction
{
public:
    virtual ~YieldInstruction() = default;

    /// Called each frame. Returns true when the coroutine should resume.
    virtual bool IsReady(float deltaTime) = 0;

    /// Reset the instruction for reuse (e.g., in repeating coroutines).
    virtual void Reset() {}
};
```

### WaitForSeconds

Pauses execution for a specified duration in seconds. The instruction accumulates elapsed time each frame via `IsReady(deltaTime)` and resumes once the elapsed time meets or exceeds the target.

```cpp
class WaitForSeconds : public YieldInstruction
{
public:
    explicit WaitForSeconds(float seconds);
    bool IsReady(float deltaTime) override;
    void Reset() override;
};
```

**Parameters:**
- `seconds` -- The number of seconds to wait before resuming.

**Behavior:** Accumulates `deltaTime` across frames. Returns `true` from `IsReady()` once `m_elapsed >= m_target`.

### WaitForFrames

Pauses execution for a specified number of frames. Each call to `IsReady()` increments an internal counter, and the instruction becomes ready when the count reaches the target.

```cpp
class WaitForFrames : public YieldInstruction
{
public:
    explicit WaitForFrames(int frames);
    bool IsReady(float deltaTime) override;
    void Reset() override;
};
```

**Parameters:**
- `frames` -- The number of frames to wait. The coroutine resumes on the frame where the count reaches this value.

### WaitUntil

Pauses execution until a user-supplied predicate returns `true`. The predicate is evaluated every frame.

```cpp
class WaitUntil : public YieldInstruction
{
public:
    explicit WaitUntil(std::function<bool()> predicate);
    bool IsReady(float deltaTime) override;
};
```

**Parameters:**
- `predicate` -- A callable returning `bool`. The coroutine resumes when it returns `true`.

**Note:** The predicate is called every frame, so keep it lightweight. Avoid expensive computations or I/O inside the predicate.

### WaitForEndOfFrame

Pauses execution for exactly one frame. The coroutine resumes on the next `Update()` call after the one that first encounters this instruction.

```cpp
class WaitForEndOfFrame : public YieldInstruction
{
public:
    bool IsReady(float deltaTime) override;
    void Reset() override;
};
```

**Behavior:** Returns `false` on the first call to `IsReady()`, then `true` on all subsequent calls. This guarantees at least one frame of delay.

### WaitForEvent

Pauses execution until a shared atomic boolean flag is set to `true`. This is the primary mechanism for integrating coroutines with the `EventBus` system without introducing a direct include dependency.

```cpp
class WaitForEvent : public YieldInstruction
{
public:
    explicit WaitForEvent(std::shared_ptr<std::atomic<bool>> eventFlag);
    bool IsReady(float deltaTime) override;
    void Reset() override;
};
```

**Parameters:**
- `eventFlag` -- A shared pointer to an atomic boolean. The coroutine resumes when this flag is `true`.

**Memory ordering:** Uses `memory_order_acquire` for reading and `memory_order_release` for resetting, ensuring correct synchronization if the flag is set from another thread.

**Reset behavior:** Calling `Reset()` sets the flag back to `false`, which is useful for repeating coroutines that need to wait for the same event multiple times.

---

## Builder-Pattern API

The builder-pattern API lets you describe a coroutine as a chain of `Do()` actions and yield steps using fluent method syntax. This is ideal for simple linear sequences of actions separated by delays.

### Coroutine Class

```cpp
class Coroutine
{
public:
    explicit Coroutine(const std::string& name);

    const std::string& GetName() const;
    bool IsFinished() const;
    bool IsCancelled() const;
    void Cancel();
    void Update(float deltaTime);

    // Builder methods (all return *this for chaining)
    Coroutine& Do(std::function<void()> action);
    Coroutine& WaitForSeconds(float seconds);
    Coroutine& WaitForFrames(int frames);
    Coroutine& WaitUntil(std::function<bool()> predicate);
    Coroutine& WaitForEvent(std::shared_ptr<std::atomic<bool>> eventFlag);
    Coroutine& YieldFrame();
};
```

### Builder Methods

| Method | Description |
|--------|-------------|
| `Do(action)` | Enqueue an action step. Runs immediately when reached (no delay). |
| `WaitForSeconds(seconds)` | Enqueue a time-based yield. Resumes after `seconds` have elapsed. |
| `WaitForFrames(frames)` | Enqueue a frame-count yield. Resumes after `frames` update ticks. |
| `WaitUntil(predicate)` | Enqueue a predicate yield. Resumes when `predicate()` returns `true`. |
| `WaitForEvent(flag)` | Enqueue an event yield. Resumes when the shared atomic bool is `true`. |
| `YieldFrame()` | Enqueue a single-frame yield. Equivalent to `WaitForFrames(1)` but uses `WaitForEndOfFrame` internally. |

**Execution model:** When `Update(deltaTime)` is called, the coroutine processes steps sequentially. Action steps execute immediately and the coroutine advances to the next step in the same frame. Yield steps check their condition; if satisfied, the coroutine advances. If not, processing stops until the next frame. This means multiple consecutive `Do()` steps all execute in a single frame, while yield steps introduce pauses.

### Builder Examples

**Simple delayed action:**

```cpp
auto& scheduler = Spark::CoroutineScheduler::GetInstance();

scheduler.StartCoroutine("greet")
    .Do([]() { Logger::Info("Hello..."); })
    .WaitForSeconds(2.0f)
    .Do([]() { Logger::Info("...World!"); });
```

**Multi-step sequence with different yield types:**

```cpp
scheduler.StartCoroutine("intro_sequence")
    .Do([&]() { camera.FadeToBlack(); })
    .WaitForSeconds(1.0f)
    .Do([&]() { camera.SetPosition(introPos); })
    .YieldFrame()  // let the renderer pick up the new position
    .Do([&]() { camera.FadeFromBlack(); })
    .WaitForSeconds(2.0f)
    .Do([&]() { hud.ShowObjective("Find the exit"); });
```

**Waiting for a condition:**

```cpp
bool doorUnlocked = false;

scheduler.StartCoroutine("wait_for_door")
    .Do([&]() { hud.ShowPrompt("Find the key"); })
    .WaitUntil([&]() { return doorUnlocked; })
    .Do([&]() { door.Open(); hud.HidePrompt(); });
```

---

## C++20 Coroutine API

For more complex asynchronous logic involving loops, conditionals, or deeply nested sequences, the C++20 coroutine API provides a natural programming model using `co_await`, `co_yield`, and `co_return`.

### GameCoroutine Return Type

Any function that returns `GameCoroutine` can use coroutine keywords. The `GameCoroutine` object owns the coroutine handle and destroys it in its destructor (RAII).

```cpp
class GameCoroutine
{
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    GameCoroutine();
    explicit GameCoroutine(handle_type h);

    // Move-only (no copy)
    GameCoroutine(GameCoroutine&& other) noexcept;
    GameCoroutine& operator=(GameCoroutine&& other) noexcept;
    ~GameCoroutine();

    bool IsFinished() const;
    bool Update(float deltaTime);
    handle_type GetHandle() const;
};
```

**Key properties:**
- **Move-only:** `GameCoroutine` cannot be copied. Use `std::move()` when passing to the scheduler.
- **RAII:** The destructor calls `m_handle.destroy()` if the handle is valid, preventing coroutine frame leaks.
- **Initial suspend:** Coroutines start in a suspended state (`initial_suspend` returns `suspend_always`). They do not execute any code until the first `Update()` call.

### Promise Type

The `promise_type` nested inside `GameCoroutine` drives the C++20 coroutine machinery:

```cpp
struct promise_type
{
    std::unique_ptr<YieldInstruction> currentYield;
    bool finished = false;

    GameCoroutine get_return_object();
    std::suspend_always initial_suspend() noexcept;
    std::suspend_always final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
    std::suspend_always yield_value(std::nullptr_t);
};
```

- `initial_suspend()` returns `suspend_always` -- coroutines do not auto-start.
- `final_suspend()` returns `suspend_always` and sets `finished = true`.
- `return_void()` sets `finished = true` (coroutines must use `co_return;` or fall off the end).
- `unhandled_exception()` sets `finished = true` (exceptions terminate the coroutine silently).
- `yield_value(nullptr)` supports `co_yield nullptr` for yielding one frame.

### Supported co_await Expressions

The following `operator co_await` overloads are provided, each creating a `YieldAwaiter<T>` that stores the yield instruction in the promise:

| Expression | Effect |
|------------|--------|
| `co_await Spark::WaitForSeconds(2.0f)` | Suspend for 2 seconds |
| `co_await Spark::WaitForFrames(5)` | Suspend for 5 frames |
| `co_await Spark::WaitUntil([&]{ return ready; })` | Suspend until predicate is true |
| `co_await Spark::WaitForEndOfFrame()` | Suspend for one frame |
| `co_await Spark::WaitForEvent(flag)` | Suspend until event flag is set |
| `co_yield nullptr` | Suspend for one frame (shorthand) |
| `co_return` | Complete the coroutine |

The `YieldAwaiter<T>` template (constrained with `std::derived_from<T, YieldInstruction>`) bridges yield instructions to the C++20 awaiter protocol:

```cpp
template <typename T>
    requires std::derived_from<T, YieldInstruction>
struct YieldAwaiter
{
    T instruction;
    explicit YieldAwaiter(T inst);
    bool await_ready() const noexcept;      // always returns false
    void await_suspend(std::coroutine_handle<GameCoroutine::promise_type> handle) const;
    void await_resume() const noexcept;
};
```

### C++20 Examples

**Basic timed sequence:**

```cpp
Spark::GameCoroutine FlashDamageIndicator(HUD& hud)
{
    hud.SetDamageFlash(1.0f);
    co_await Spark::WaitForSeconds(0.2f);
    hud.FadeDamageFlash(1.0f);
    co_await Spark::WaitForSeconds(1.0f);
    hud.SetDamageFlash(0.0f);
    co_return;
}

// Schedule it:
Spark::CoroutineScheduler::GetInstance().Schedule("damage_flash",
    FlashDamageIndicator(hud));
```

**Loop with delay (wave spawner):**

```cpp
Spark::GameCoroutine SpawnWaves(WaveManager& waves, int count)
{
    for (int i = 0; i < count; ++i)
    {
        waves.SpawnWave(i);
        co_await Spark::WaitForSeconds(10.0f);
    }
    co_return;
}
```

**Conditional logic:**

```cpp
Spark::GameCoroutine PatrolAndChase(AIController& ai)
{
    while (ai.IsAlive())
    {
        // Patrol until player is spotted
        ai.StartPatrol();
        co_await Spark::WaitUntil([&]() { return ai.CanSeePlayer(); });

        // Chase the player
        ai.StartChase();
        co_await Spark::WaitUntil([&]() { return !ai.CanSeePlayer(); });

        // Lost the player, wait before resuming patrol
        ai.StopChase();
        co_await Spark::WaitForSeconds(3.0f);
    }
    co_return;
}
```

**Using co_yield for frame-by-frame work:**

```cpp
Spark::GameCoroutine SmoothLerp(Transform& transform, Vector3 target, float duration)
{
    Vector3 start = transform.GetPosition();
    float elapsed = 0.0f;

    while (elapsed < duration)
    {
        float t = elapsed / duration;
        transform.SetPosition(Vector3::Lerp(start, target, t));
        elapsed += Time::GetDeltaTime();
        co_yield nullptr;  // wait one frame
    }

    transform.SetPosition(target);
    co_return;
}
```

---

## CoroutineScheduler

The `CoroutineScheduler` is a singleton that manages all active coroutines (both builder-pattern and C++20 native). It should be ticked once per frame from the game loop.

### Singleton Access

```cpp
Spark::CoroutineScheduler& scheduler = Spark::CoroutineScheduler::GetInstance();
```

The instance is created on first access (Meyer's singleton) and lives for the duration of the program.

### Scheduler Methods

#### StartCoroutine

```cpp
Coroutine& StartCoroutine(const std::string& name);
```

Creates a new builder-pattern coroutine with the given name and returns a reference for chaining `Do()`/`Wait` calls. The coroutine begins executing on the next `Update()` call.

**Parameters:**
- `name` -- A string identifier used for cancellation and debugging. Does not need to be unique; multiple coroutines can share a name (all will be cancelled by `StopCoroutine`).

#### Schedule

```cpp
void Schedule(const std::string& name, GameCoroutine coroutine);
```

Registers a C++20 `GameCoroutine` with the scheduler. The coroutine is wrapped in a `NativeCoroutineWrapper` for unified management.

**Parameters:**
- `name` -- A string identifier for cancellation and debugging.
- `coroutine` -- A `GameCoroutine` object (must be moved in, as it is move-only).

#### StopCoroutine

```cpp
void StopCoroutine(const std::string& name);
```

Cancels all coroutines (both builder and native) that match the given name. Cancelled coroutines are removed during the next `Update()` call.

#### StopAll

```cpp
void StopAll();
```

Cancels every active coroutine. Useful for scene transitions or shutdown.

#### IsRunning

```cpp
bool IsRunning(const std::string& name) const;
```

Returns `true` if at least one non-cancelled, non-finished coroutine with the given name exists.

#### ActiveCount

```cpp
size_t ActiveCount() const;
```

Returns the total number of active coroutines (builder + native combined). Finished and cancelled coroutines are not counted after the next `Update()` removes them.

#### Update

```cpp
void Update(float deltaTime);
```

Ticks all active coroutines forward by one frame. This method:

1. Calls `Update(deltaTime)` on each active builder-pattern coroutine.
2. Calls `Update(deltaTime)` on each active native coroutine wrapper.
3. Removes all finished or cancelled coroutines from both lists (erase-remove idiom).

### Update Loop Integration

Call `CoroutineScheduler::Update()` once per frame from your game loop, after physics and before rendering:

```cpp
void GameLoop(float deltaTime)
{
    // Physics, AI, etc.
    PhysicsSystem::Update(deltaTime);
    AISystem::Update(deltaTime);

    // Coroutines
    Spark::CoroutineScheduler::GetInstance().Update(deltaTime);

    // Rendering
    GraphicsEngine::Render();
}
```

---

## NativeCoroutineWrapper

The `NativeCoroutineWrapper` class adapts a `GameCoroutine` so the scheduler can manage it with the same interface as builder-pattern coroutines.

```cpp
class NativeCoroutineWrapper
{
public:
    NativeCoroutineWrapper(const std::string& name, GameCoroutine coroutine);

    const std::string& GetName() const;
    bool IsFinished() const;
    bool IsCancelled() const;
    void Cancel();
    void Update(float deltaTime);
};
```

You do not normally interact with this class directly. It is used internally by `CoroutineScheduler::Schedule()`.

---

## Convenience Free Functions

Three free functions in the `Spark` namespace provide shorthand access to the global scheduler:

```cpp
/// Start a builder-pattern coroutine on the global scheduler.
Spark::Coroutine& Spark::StartCoroutine(const std::string& name);

/// Stop a coroutine by name on the global scheduler.
void Spark::StopCoroutine(const std::string& name);

/// Schedule a C++20 coroutine on the global scheduler.
void Spark::ScheduleCoroutine(const std::string& name, GameCoroutine coroutine);
```

**Usage:**

```cpp
// Builder pattern via free function
Spark::StartCoroutine("flash")
    .Do([]() { /* ... */ })
    .WaitForSeconds(1.0f)
    .Do([]() { /* ... */ });

// C++20 via free function
Spark::ScheduleCoroutine("waves", SpawnWaves(5));

// Cancel via free function
Spark::StopCoroutine("flash");
```

---

## Integration with EventBus

The `WaitForEvent` yield instruction uses a `std::shared_ptr<std::atomic<bool>>` flag to decouple the coroutine system from the `EventBus` include hierarchy. To wait for an event inside a coroutine:

1. Create a shared atomic boolean flag.
2. Subscribe to the event on the `EventBus`, setting the flag to `true` in the handler.
3. Use `WaitForEvent(flag)` in the coroutine.

### Builder-Pattern Example

```cpp
auto& scheduler = Spark::CoroutineScheduler::GetInstance();
auto& eventBus = EngineContext::GetEventBus();

// Create a shared flag
auto doorOpenFlag = std::make_shared<std::atomic<bool>>(false);

// Subscribe to the event
auto subID = eventBus.Subscribe<DoorOpenEvent>(
    [doorOpenFlag](const DoorOpenEvent& e)
    {
        doorOpenFlag->store(true, std::memory_order_release);
    });

// Coroutine waits for the event
scheduler.StartCoroutine("on_door_open")
    .WaitForEvent(doorOpenFlag)
    .Do([&]()
    {
        PlaySound("door_creak");
        TriggerCutscene("enter_dungeon");
    });
```

### C++20 Example

```cpp
Spark::GameCoroutine WaitForDoorAndEnter(
    EventBus& eventBus,
    std::shared_ptr<std::atomic<bool>> doorFlag)
{
    co_await Spark::WaitForEvent(doorFlag);
    PlaySound("door_creak");
    co_await Spark::WaitForSeconds(1.5f);
    TriggerCutscene("enter_dungeon");
    co_return;
}

// Setup and schedule:
auto doorFlag = std::make_shared<std::atomic<bool>>(false);
eventBus.Subscribe<DoorOpenEvent>(
    [doorFlag](const DoorOpenEvent&) { doorFlag->store(true); });
Spark::ScheduleCoroutine("door_enter", WaitForDoorAndEnter(eventBus, doorFlag));
```

**Important:** Remember to unsubscribe from the `EventBus` when the coroutine completes or when the flag is no longer needed, to avoid dangling subscriptions.

---

## Practical Examples

### Damage Flash HUD Effect

A common FPS pattern: flash the screen red when the player takes damage, then fade it out.

```cpp
// Builder pattern
Spark::StartCoroutine("damage_flash")
    .Do([&]() { hud.SetDamageFlash(1.0f); })
    .WaitForSeconds(0.2f)
    .Do([&]() { hud.FadeDamageFlash(1.0f); })
    .WaitForSeconds(1.0f)
    .Do([&]() { hud.SetDamageFlash(0.0f); });
```

### Enemy Wave Spawner

Spawn multiple waves with a delay between each, using C++20 coroutines for natural loop syntax.

```cpp
Spark::GameCoroutine SpawnWaves(WaveManager& waves, int waveCount)
{
    for (int i = 0; i < waveCount; ++i)
    {
        waves.SpawnWave(i);
        Logger::Info("Wave {} spawned", i + 1);
        co_await Spark::WaitForSeconds(10.0f);
    }
    Logger::Info("All waves complete");
    co_return;
}

Spark::ScheduleCoroutine("waves", SpawnWaves(waveManager, 5));
```

### Door Open Sequence

A multi-step scripted sequence combining delays, frame yields, and actions.

```cpp
Spark::StartCoroutine("door_sequence")
    .Do([&]() { player.DisableInput(); })
    .Do([&]() { camera.LookAt(door.GetPosition()); })
    .YieldFrame()
    .Do([&]() { door.PlayAnimation("open"); })
    .WaitForSeconds(1.5f)
    .Do([&]() { camera.ResetToPlayer(); })
    .YieldFrame()
    .Do([&]() { player.EnableInput(); });
```

### Countdown Timer

Display a countdown before a match starts.

```cpp
Spark::GameCoroutine MatchCountdown(HUD& hud)
{
    for (int i = 3; i > 0; --i)
    {
        hud.ShowCenterText(std::to_string(i));
        co_await Spark::WaitForSeconds(1.0f);
    }
    hud.ShowCenterText("GO!");
    co_await Spark::WaitForSeconds(0.5f);
    hud.HideCenterText();
    co_return;
}
```

### Waiting for Player Input

Use `WaitUntil` to pause a tutorial sequence until the player performs an action.

```cpp
Spark::StartCoroutine("tutorial_step1")
    .Do([&]() { hud.ShowTutorial("Press SPACE to jump"); })
    .WaitUntil([&]() { return input.WasKeyPressed(Key::Space); })
    .Do([&]() { hud.ShowTutorial("Great! Now press SHIFT to sprint"); })
    .WaitUntil([&]() { return input.WasKeyPressed(Key::Shift); })
    .Do([&]() { hud.HideTutorial(); });
```

---

## Threading Model

The coroutine system is **main-thread only**. All coroutine execution, yield checking, and scheduler updates happen on the thread that calls `CoroutineScheduler::Update()`. This means:

- Coroutine actions can safely access game state without synchronization.
- The `WaitForEvent` flag can be set from any thread (it uses `std::atomic<bool>` with appropriate memory ordering), but the coroutine itself resumes on the main thread.
- Do not call `Update()` from multiple threads simultaneously.
- Do not store references to `Coroutine&` across threads.

---

## Testing

The coroutine system is covered by `Tests/TestCoroutineScheduler.cpp` with 10 test cases:

| Test Case | Description |
|-----------|-------------|
| `Coroutine_DoExecutesImmediately` | Verifies that consecutive `Do()` actions execute in a single `Update()` call |
| `Coroutine_WaitForSecondsDelays` | Verifies time-based yielding with accumulated delta time |
| `Coroutine_WaitForFramesDelays` | Verifies frame-count yielding over multiple updates |
| `Coroutine_WaitUntilPredicate` | Verifies predicate-based yielding |
| `Coroutine_CancelStopsExecution` | Verifies that `Cancel()` prevents further step execution |
| `Scheduler_ManagesMultipleCoroutines` | Verifies concurrent coroutines with independent timelines |
| `Scheduler_StopByName` | Verifies cancellation by name through the scheduler |
| `Scheduler_StopAll` | Verifies bulk cancellation |
| `Scheduler_IsRunning` | Verifies runtime status queries |
| `Coroutine_ChainedWaits` | Verifies multiple consecutive wait-action-wait sequences |

Run tests with:

```bash
cd build && ctest --output-on-failure
```

---

## See Also

- [Event System](Event-System.md) -- `EventBus` for publish/subscribe events, used with `WaitForEvent`
- [Entity Component System](Entity-Component-System.md) -- ECS architecture and system execution order
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Higher-level gameplay logic that uses coroutines
- [Architecture Overview](../getting-started/Architecture-Overview.md) -- Overall engine architecture and subsystem relationships
- [Animation](Animation.md) -- Animation state machines that may coordinate with coroutines
- [AI and Navigation](AI-and-Navigation.md) -- AI behavior trees that can use coroutines for scripted sequences
- [Testing](../advanced/Testing.md) -- Test framework and conventions used by `TestCoroutineScheduler`
