# Utilities

SparkEngine ships a rich, header-only utility layer in `SparkEngine/Source/Utils/`. This page documents all utilities, grouped by category.

---

## Existing Utilities (pre-existing)

| File | Purpose |
|------|---------|
| `Assert.h` | SPARK_ASSERT / ASSERT_HR macros with stack traces |
| `Validate.h` | SPARK_VALIDATE / SPARK_REQUIRE defensive programming macros |
| `Logger.h` | Multi-sink async logger (file, stderr, callback) |
| `LogMacros.h` | SPARK_LOG_INFO / SPARK_LOG_ERROR convenience macros |
| `SparkConsole.h` | Thread-safe in-game debug console with CVar support |
| `ConsoleVariable.h` | Type-safe console variables with callbacks |
| `Result.h` | `Result<T>` error-propagation type |
| `StringUtils.h` | String case, trim, split, parse helpers |
| `ColorUtils.h` | Color conversion utilities |
| `FileUtils.h` | Cross-platform file read/write/list/path helpers |
| `LocalFileCache.h` | File-based asset cache |
| `ConfigParser.h` | INI-style config file parser |
| `MathUtils.h` | Constants, lerp, smooth-step, easing, random, matrix builders |
| `SplineMath.h` | Catmull-Rom and Bezier spline evaluation |
| `SplinePath.h` | Arc-length parameterization along splines |
| `Timer.h` | High-precision delta/total timer |
| `ScopedTimer.h` | RAII automatic scope timing |
| `Profiler.h` | Per-system CPU/GPU profiler with ImGui overlay |
| `DeltaSmoother.h` | Frame-time jitter smoothing |
| `PerformanceStats.h` | Frame performance statistics |
| `Tween.h` | 30+ easing functions; tweening system |
| `Cooldown.h` | Reusable cooldown timer for abilities |
| `ObjectPool.h` | Fixed-capacity O(1) object pool |
| `FrameAllocator.h` | Frame-scoped scratch allocator |
| `ThreadSafeQueue.h` | Mutex-guarded FIFO producer/consumer queue |
| `JobSystem.h` | Multi-threaded job scheduler |
| `Octree.h` | Loose octree for spatial queries |
| `BitFlags.h` | Type-safe bitmask wrapper and operator overloads |
| `RingBuffer.h` | Circular fixed-size buffer |
| `OpaqueHandle.h` | Type-safe opaque handle wrappers |
| `UUID.h` | 64-bit UUID generation and serialization |
| `RandomEngine.h` | `mt19937`-backed random with distributions |
| `DebugDraw.h` | Immediate-mode 3D debug line/shape rendering |
| `StackTrace.h` | Stack trace capture and formatting |
| `ChromeTracing.h` | Chrome DevTools trace format export |
| `D3DUtils.h` | DirectX 11 COM and HRESULT helpers |

---

## New Utilities (added 2026)

### `TypeTraits.h` — C++23 concept shorthands and type utilities

Provides reusable C++23 concept aliases and `TypeList<>` utilities.

**Concepts:**

| Concept | Satisfied by |
|---------|-------------|
| `Arithmetic<T>` | `int`, `float`, `double`, etc. |
| `Enum<T>` | Any scoped or unscoped enum |
| `StringLike<T>` | `std::string`, `std::string_view`, `const char*` |
| `TriviallyCopyable<T>` | Trivially copyable types (safe for `memcpy`) |
| `EqualityComparable<T>` | Types with `operator==` |
| `Comparable<T>` | Totally-ordered types |
| `Callable<F, Ret, Args...>` | Callable with specific signature |
| `Invocable<F, Args...>` | Callable with any return type |
| `Pointer<T>` | Raw pointer types |
| `DefaultConstructible<T>` | Default-initializable types |

**Type utilities:**

| Utility | Purpose |
|---------|---------|
| `TypeList<Ts...>` | Compile-time ordered type list |
| `TypeIndex<T, List>` | Zero-based index of `T` in a `TypeList` |
| `TypeAt<N, List>` | Type at index `N` in a `TypeList` |
| `AlwaysFalse<T>` | `false` dependent on `T` (for `static_assert` in dead branches) |
| `RemoveCVRef<T>` | Strip `const`, `volatile`, and `&`/`&&` |
| `IsOneOf<T, Ts...>` | `true` if `T` matches any of `Ts...` |

```cpp
// Constrain templates
template <Spark::Arithmetic T> T Clamp(T v, T lo, T hi) { return std::clamp(v, lo, hi); }

// Compile-time type indexing
using Components = Spark::TypeList<Transform, Velocity, Health>;
constexpr size_t idx = Spark::TypeIndex<Velocity, Components>::value; // 1
```

---

### `Hash.h` — Compile-time and runtime FNV-1a hashing

**Functions:**

| Function | Description |
|----------|-------------|
| `FNV1a32(str)` | 32-bit FNV-1a hash (`const char*`, `string_view`, or raw bytes) |
| `FNV1a64(str)` | 64-bit FNV-1a hash (`const char*`, `string_view`, or raw bytes) |
| `CombineHash(seed, value)` | Fold a hash value into a seed (in-place or returning) |

**User-defined literals** (via `using namespace Spark::HashLiterals`):

| Literal | Result |
|---------|--------|
| `"name"_hash64` | `constexpr uint64_t` |
| `"name"_hash32` | `constexpr uint32_t` |

```cpp
using namespace Spark::HashLiterals;

// Zero-cost compile-time dispatch
switch (Spark::FNV1a64(commandName)) {
    case "quit"_hash64:   DoQuit();   break;
    case "reload"_hash64: DoReload(); break;
}

// Composite key for unordered_map
size_t key = 0;
Spark::CombineHash(key, std::hash<int>{}(x));
Spark::CombineHash(key, std::hash<int>{}(y));
```

---

### `ScopeGuard.h` — RAII scope-exit cleanup helpers

Three guard types for deterministic resource cleanup:

| Class | When does the callback fire? |
|-------|------------------------------|
| `ScopeExit` | Always, on any scope exit |
| `ScopeSuccess` | Only when the scope exits normally (no exception) |
| `ScopeFail` | Only when the scope exits via an exception |

All guards support `Dismiss()` (prevent firing), `Arm()` (re-enable), `IsActive()` query.
Use the factory helpers `MakeScopeExit`, `MakeScopeSuccess`, `MakeScopeFail` to avoid spelling the lambda type.

```cpp
// Release a COM object regardless of how the scope exits
auto cleanup = Spark::MakeScopeExit([&]{ if (buf) buf->Release(); });

// Roll back only on failure
db.BeginTransaction();
auto rollback = Spark::MakeScopeFail([&]{ db.Rollback(); });
DoWork();
db.Commit();
rollback.Dismiss(); // success — don't roll back
```

---

### `Serializer.h` — Binary serialization / deserialization

**`BinaryWriter`** — append-only binary buffer:

| Method | Description |
|--------|-------------|
| `Write<T>(value)` | Write a trivially-copyable value (little-endian) |
| `WriteString(str)` | Write a `uint32_t` length-prefix + UTF-8 bytes |
| `WriteBytes(ptr, size)` | Write raw bytes (no endian conversion) |
| `GetBuffer()` | Const reference to the internal buffer |
| `TakeBuffer()` | Move the buffer out |
| `Size()` | Current byte count |
| `Clear()` | Reset to empty |

**`BinaryReader`** — sequential, bounds-checked reader:

| Method | Description |
|--------|-------------|
| `Read<T>()` | Read a value (zero-initialised and error-flagged on overflow) |
| `ReadString()` | Read a length-prefixed string |
| `ReadBytes(dest, size)` | Copy raw bytes |
| `Skip(count)` | Advance cursor without reading |
| `Tell()` | Current cursor position |
| `Remaining()` | Bytes left to read |
| `HasError()` | `true` if any read overflowed |
| `IsEOF()` | `true` at end of buffer with no error |

```cpp
Spark::BinaryWriter w;
w.Write<uint32_t>(version);
w.WriteString(levelName);
SaveToFile(w.GetBuffer());

Spark::BinaryReader r(fileBytes);
auto ver  = r.Read<uint32_t>();
auto name = r.ReadString();
if (r.HasError()) { /* truncated */ }
```

---

### `EventBus.h` — Type-safe publish/subscribe event bus

A thread-safe, type-erased event bus. Handlers are invoked synchronously on the publishing thread.

**`EventBus` methods:**

| Method | Description |
|--------|-------------|
| `Subscribe<E>(handler)` | Register a handler; returns a `SubscriptionHandle` |
| `Publish<E>(event)` | Invoke all handlers for event type `E` |
| `ClearSubscribers<E>()` | Remove all handlers for `E` |
| `ClearAll()` | Remove all handlers for all types |
| `SubscriberCount<E>()` | Query active subscriber count for `E` |
| `EventBus::Global()` | Access the engine-wide singleton bus |

**`SubscriptionHandle`** — RAII handle that auto-unsubscribes on destruction.

| Method | Description |
|--------|-------------|
| `Unsubscribe()` | Explicitly remove the subscription |
| `IsActive()` | `true` while the subscription is live |

```cpp
struct PlayerDiedEvent { int playerId; };

// Subscribe (auto-unsubscribes when handle is destroyed)
auto h = Spark::EventBus::Global().Subscribe<PlayerDiedEvent>(
    [](const PlayerDiedEvent& e) { RespawnPlayer(e.playerId); });

// Publish from anywhere
Spark::EventBus::Global().Publish<PlayerDiedEvent>({ .playerId = 7 });
```

---

### `StateMachine.h` — Generic template finite state machine

A lightweight FSM parameterized on a state ID type (typically an `enum class`).

**`StateMachine<StateID>` API:**

| Method | Description |
|--------|-------------|
| `AddState(id, onEnter, onUpdate, onExit)` | Register a state with optional callbacks |
| `AddTransition(from, to, condition)` | Register a conditional auto-transition |
| `Start(initialState)` | Activate the machine; calls `onEnter` |
| `Stop()` | Deactivate; calls `onExit` on current state |
| `Tick(dt)` | Evaluate transitions, then call `onUpdate(dt)` |
| `TransitionTo(id)` | Force an immediate state change |
| `GetCurrentState()` | Returns `std::optional<StateID>` |
| `IsInState(id)` | Predicate: currently in `id`? |
| `IsRunning()` | Whether the machine is active |
| `HasState(id)` | Whether `id` is registered |
| `Reset()` | Stop and clear all states/transitions |

```cpp
enum class EnemyState { Idle, Chase, Attack };
Spark::StateMachine<EnemyState> fsm;

fsm.AddState(EnemyState::Idle,
    []{ PlayAnim("idle"); },
    [](float dt){ WaitForPlayer(dt); });

fsm.AddState(EnemyState::Chase,
    []{ PlayAnim("run"); },
    [&](float dt){ MoveToward(player, dt); });

fsm.AddTransition(EnemyState::Idle, EnemyState::Chase,
    [&]{ return Distance(enemy, player) < 20.0f; });

fsm.Start(EnemyState::Idle);
// Each frame:
fsm.Tick(dt);
```

---

## Thread Safety Summary

| Utility | Thread safety |
|---------|--------------|
| `TypeTraits.h` | Fully `constexpr` / compile-time; no runtime state |
| `Hash.h` | Stateless functions — safe from any thread |
| `ScopeGuard.h` | Not thread-safe; use per-scope (stack only) |
| `Serializer.h` | Not thread-safe; one reader/writer per thread |
| `EventBus.h` | Thread-safe (per-channel mutex); no recursive publish |
| `StateMachine.h` | Not thread-safe; single-thread use |
