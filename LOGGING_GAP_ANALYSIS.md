# SparkEngine Logging System — Gap Analysis

## Context

This analysis identifies gaps in SparkEngine's logging infrastructure and logging coverage across all subsystems. The engine has **four separate, disconnected logging systems** and the vast majority of subsystems have **zero logging calls**. The goal is a complete rewrite into a unified logging system that is used pervasively and logically throughout the codebase.

### Current Logging Inventory

| System | Location | Output | Used By |
|--------|----------|--------|---------|
| `SimpleConsole` + `LOG_TO_CONSOLE` macros | `Utils/SparkConsole.h`, `Utils/LogMacros.h` | Win32 console window | GraphicsEngine, Shader, Camera, SceneManager, Game, Player, ProjectilePool |
| `SparkError::LogMessage` + `SPARK_LOG_*` macros | `Utils/SparkError.h` | stderr + `OutputDebugStringA` | GraphicsEngine, AudioEngine, Game, PlaceholderMesh, StackTrace (6 files, 35 calls) |
| `FileLogger` + `SPARK_FILE_LOG_*` macros | `Utils/FileLogger.h` | Timestamped log files | **Not used anywhere** (only in test file) |
| `EditorLogger` + `SE_LOG_*` macros | `SparkEditor/Core/EditorLogger.h` | Console, file, memory buffer | Editor UI, EditorApplication, main.cpp (4 files) |

**Total files with any logging: 22 out of 340 source files (6.5%)**

---

## Critical Gaps (Architecture)

### 1. Four Disconnected Logging Systems — No Unified Logger

- **Problem**: Four completely independent logging systems with different APIs, severity enums, output sinks, and thread-safety models. None of them route to each other.
  - `SimpleConsole::Log()` uses string-based types (`"INFO"`, `"ERROR"`) and wide-string macros
  - `SparkError::LogMessage()` uses `SparkError::Severity` enum and printf-style formatting
  - `FileLogger::Write()` uses `Spark::FileLogLevel` enum (duplicate of SparkError's)
  - `EditorLogger::Log()` uses `SparkEditor::LogLevel` enum (yet another duplicate)
- **Impact**: A `SPARK_LOG_ERROR` call never appears in the SimpleConsole, the FileLogger, or the EditorLogger. Developers must choose which system to log to and most choose none.
- **Fix**: Replace all four with a single `Spark::Logger` class that dispatches to multiple sinks (console, file, debug output, editor memory buffer).

### 2. Three Duplicate Severity Enums

- **Files**: `SparkError.h` (`SparkError::Severity`), `FileLogger.h` (`Spark::FileLogLevel`), `EditorLogger.h` (`SparkEditor::LogLevel`)
- **Problem**: Three nearly identical severity enums with slightly different naming (`Warn` vs `WARNING`, `Error` vs `ERROR_`, `Fatal` vs `CRITICAL`). The `ERROR_` rename in EditorLogger is a workaround for a Windows macro collision.
- **Impact**: Cannot pass severity levels between systems; no unified filtering.
- **Fix**: Single `Spark::LogLevel` enum used everywhere, with `SPARK_LOG_LEVEL_ERROR` naming to avoid the Windows `ERROR` macro.

### 3. `FileLogger` Is Dead Code

- **File**: `Utils/FileLogger.h` (432 lines)
- **Problem**: Fully implemented file logger with rotation, CSV output, session headers — but never initialized or called anywhere in the engine. Only referenced in `Tests/TestDebugTools.cpp`.
- **Impact**: No persistent log files are ever written during engine execution. Crash diagnosis requires attaching a debugger.
- **Fix**: Integrate file output as a sink in the unified logger; auto-initialize on engine startup.

### 4. `LOG_TO_CONSOLE` Macros Use Broken Wide-String Conversion

- **File**: `Utils/LogMacros.h` (lines 37-41)
- **Problem**: `std::string strMsg(wstrMsg.begin(), wstrMsg.end())` is a lossy truncation, not a proper encoding conversion. Any non-ASCII character (e.g., file paths with accents, Unicode player names) will be corrupted.
- **Impact**: Silent data corruption in log messages on any non-English system.
- **Fix**: Unified logger should use `std::string` (UTF-8) everywhere. Remove the wide-string macro layer entirely.

### 5. No Runtime Log Level Filtering

- **Problem**: `SPARK_LOG_*` macros in `SparkError.h` always call `LogMessage()` — there is no minimum-level check. Every trace-level message goes through mutex lock + `fprintf` + `OutputDebugStringA` regardless of build config.
- **Impact**: Performance penalty from logging in hot paths; cannot dial up/down verbosity at runtime.
- **Fix**: Add a global/per-category minimum level that is checked before formatting the message.

---

## Critical Gaps (Coverage)

### 6. Engine Subsystems Have Zero Logging (12 out of 14)

The following subsystems contain **no logging calls of any kind** despite handling complex, failure-prone operations:

| Subsystem | Files | Key Operations With No Logging |
|-----------|-------|-------------------------------|
| **AI System** | `AISystem.h/cpp`, `BehaviorTree.h`, `NavMesh.h/cpp`, `PerceptionSystem.h`, `SteeringBehaviors.h` | NavMesh bake/query failures, behavior tree node transitions, perception events, pathfinding failures |
| **Animation** | `AnimationSystem.h/cpp` | Skeleton load errors, missing bone references, IK solver failures, state machine transitions |
| **Physics** | `Physics/` directory | Collision detection, rigidbody creation, constraint errors, simulation step anomalies |
| **Networking** | `NetworkManager.h/cpp` | Connection open/close, message send/receive, deserialization errors, timeout events |
| **Save System** | `SaveSystem.h/cpp` | File I/O errors, serialization failures, version mismatches, corrupt save detection |
| **ECS** | `ECS/Systems/ECSystems.h/cpp` | Entity creation/destruction, component add/remove, system registration |
| **Event System** | `Events/EventSystem.h` | Event dispatch, handler registration, unhandled events |
| **Scripting** | `Scripting/AngelScriptEngine.h` | Script compilation errors, runtime exceptions, API binding failures |
| **Procedural Gen** | `Procedural/ProceduralGeneration.h/cpp` | Generation parameters, seed values, chunk creation, failures |
| **Cinematic** | `Cinematic/Sequencer.h/cpp` | Sequence play/stop, keyframe evaluation, track binding errors |
| **Coroutine** | `Coroutine/CoroutineScheduler.h` | Coroutine start/stop, yield points, exception in coroutine |
| **Input** | `Input/` directory | Device connection/disconnection, binding changes, input mode switches |

### 7. Graphics Uses `LOG_TO_CONSOLE` But Not `SPARK_LOG_*`

- **File**: `Graphics/GraphicsEngine.cpp` (177 `LOG_TO_CONSOLE` calls, 8 `SPARK_LOG_*` calls)
- **Problem**: The vast majority of graphics logging goes only to the SimpleConsole (which requires the console window to be open). Errors during DX11 device creation, shader compilation, and render target setup are invisible unless the console window happens to be visible.
- **Impact**: Graphics initialization failures are silently swallowed in release builds without the console.

### 8. Audio Has Minimal Logging (5 Calls Total)

- **File**: `Audio/AudioEngine.cpp`
- **Problem**: Only 5 `SPARK_LOG_*` calls in the entire audio subsystem. No logging for: XAudio2 device creation, voice pool exhaustion, buffer underruns, sound file load failures, 3D audio calculation errors.
- **Impact**: Audio issues are extremely difficult to diagnose.

### 9. Core Engine Startup/Shutdown Partially Logged

- **Files**: `Core/SparkEngine.cpp` (64 console.Log calls), `Core/ModuleManager.cpp` (21 calls)
- **Problem**: SparkEngine.cpp and ModuleManager.cpp use `console.Log*()` directly (not macros), coupling them to SimpleConsole initialization order. If the console fails to init, all startup logging is lost.
- **Impact**: Bootstrap failures produce no diagnostic output.

---

## Major Gaps

### 10. Raw `printf`/`std::cout` Used Instead of Logging (120+ Calls)

- **Files**: 23 source files use raw `printf`, `fprintf(stderr)`, `std::cout`, or `std::cerr`
- **Key offenders**:
  - `Graphics/MaterialSystem.cpp` (24 `printf` calls)
  - `Utils/MemoryDebugger.h` (17 `printf` calls)
  - `Graphics/GraphicsEngine.cpp` (12 `printf` calls alongside 177 `LOG_TO_CONSOLE`)
  - `Utils/SparkConsole.cpp` (20 `printf` calls — the logger itself uses printf)
  - `Enums/EnumTests.h` (11 `printf` calls)
  - `Utils/DebugOverlay.h` (8 `printf` calls)
  - `Graphics/RHI/RHIFactory.cpp` (6 `printf` calls)
- **Impact**: These bypass all logging infrastructure — no timestamps, no severity, no filtering, no file output.

### 11. `OutputDebugStringA` Used Directly (12 Files)

- **Files**: `InstanceRenderer.h`, `ConsoleApp.cpp`, `Platform.h`, `Model.cpp`, `Mesh.cpp`, `Shader.cpp`, `ConsoleProcessManager.cpp`, `CrashHandler.cpp`, `SparkConsole.cpp`, `SparkError.h`, `Console.h`
- **Problem**: Direct `OutputDebugStringA` calls bypass the logging system entirely. Only visible in Visual Studio Output window, lost in production.
- **Impact**: Diagnostic messages scattered across different output channels with no way to capture them uniformly.

### 12. No Structured/Categorized Logging in Engine

- **Problem**: `SPARK_LOG_*` macros take a free-form string category (`"Graphics"`, `"Audio"`). There is no enum or compile-time validation. Typos like `"Graphcs"` would create a phantom category.
- **EditorLogger** has `LogCategory` enum but it's editor-only and not used by engine code.
- **Impact**: Cannot reliably filter by subsystem; inconsistent category strings across the codebase.

### 13. No Conditional/Periodic Logging Utility

- **Problem**: `LOG_TO_CONSOLE_RATE_LIMITED` exists but is crude (3 messages per 3 seconds, per call site via `static` variables). There is no:
  - `LOG_ONCE` — log only the first occurrence
  - `LOG_EVERY_N` — log every Nth occurrence
  - `LOG_IF` — log only when a condition is true
  - `LOG_EVERY_SECONDS` — configurable rate limit
- **Impact**: Developers either flood the console with per-frame messages or don't log at all.

### 14. Thread Safety Is Inconsistent

- `SimpleConsole`: mutex on `m_logHistory` but `Print()`/`PrintLine()` call Win32 APIs without synchronization
- `SparkError::LogMessage()`: has a `static std::mutex` — correct but uses a function-local static, meaning the lock is taken even for the severity check
- `FileLogger`: properly mutex-protected but never used
- `EditorLogger`: has `m_mutex` — correct
- **Impact**: Potential console output corruption under multithreaded logging; unnecessary mutex contention.

### 15. SimpleConsole Is Windows-Only

- **File**: `SparkConsole.h` (lines 114-121, 155-166)
- **Problem**: `Color` enum uses `FOREGROUND_*` Windows constants. `CreateConsoleWindow()` uses `AllocConsole()`. Linux path has placeholder `int` file descriptors with no implementation.
- **Impact**: All 470+ `LOG_TO_CONSOLE` calls in the codebase are non-functional on Linux/macOS.

---

## Moderate Gaps

### 16. No Log Callback / Event Hook System

- **Problem**: No way for game code or plugins to register a callback that receives all log messages. The EditorLogger has `LogTarget` but it's editor-only.
- **Impact**: Cannot integrate with external log aggregators, crash reporting services (Sentry, Crashpad), or custom in-game log displays.

### 17. No Compile-Time Log Stripping

- **Problem**: `SPARK_LOG_TRACE` and `SPARK_LOG_DEBUG` are always compiled in. In a release build of an FPS game, trace-level logging in hot paths (physics, rendering) adds unnecessary code.
- **Impact**: Binary size and potential branch-prediction overhead in release builds.

### 18. No `std::format` / C++20 Integration

- **Problem**: All logging uses C-style `printf` formatting (`snprintf`, `vsnprintf`). The engine requires C++20 but doesn't use `std::format` or `std::source_location`.
- **Impact**: Type-unsafe formatting; verbose `__FILE__`/`__LINE__`/`__FUNCTION__` macro plumbing that `std::source_location` could replace.

### 19. No Log Message Deduplication

- **Problem**: If the same error occurs every frame (e.g., missing texture), it generates thousands of identical log entries with no suppression.
- **Impact**: Log files fill up with noise; console becomes unreadable; file rotation triggers prematurely.

### 20. Fixed-Size Message Buffers

- **Files**: `SparkError.h` (line 74: `char userMsg[1024]`, line 94: `char fullMsg[2048]`), `FileLogger.h` (line 306: `char buf[2048]`)
- **Problem**: Log messages are silently truncated at ~1KB. Stack traces, serialization dumps, or long file paths can exceed this.
- **Impact**: Truncated diagnostic information precisely when detailed output is most needed.

### 21. Profiler Has No Log Integration

- **File**: `Utils/Profiler.h`
- **Problem**: The profiler tracks timing data but doesn't log performance anomalies (frame spikes, budget overruns). Data is only visible in the ImGui overlay.
- **Impact**: Cannot capture performance events in log files for post-mortem analysis.

### 22. Crash Handler Doesn't Log to File

- **Files**: `Utils/CrashHandler.cpp`, `Utils/CrashHandlerStub.cpp`
- **Problem**: `TriggerCrashHandler()` generates minidumps but doesn't flush or write the final log state to a file. Recent log history is lost on crash.
- **Impact**: Post-crash log files (if they existed) would be incomplete; the last critical messages before the crash are lost.

---

## Proposed Unified Logger Architecture

### Single Entry Point

```cpp
// New: SparkEngine/Source/Utils/Logger.h
namespace Spark
{
    enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Fatal };
    enum class LogCategory : uint8_t { Core, Graphics, Physics, Audio, AI, Animation,
                                        ECS, Network, Input, Scripting, Scene, Save,
                                        Cinematic, Procedural, Editor, Game, Count };

    class Logger
    {
    public:
        static Logger& Get();

        // Sink registration
        void AddSink(std::unique_ptr<ILogSink> sink);

        // Runtime filtering
        void SetGlobalLevel(LogLevel minimum);
        void SetCategoryLevel(LogCategory cat, LogLevel minimum);

        // Core log function
        void Log(LogLevel level, LogCategory category,
                 std::source_location loc, std::string_view message);

        // Flush all sinks (call before crash)
        void FlushAll();
    };
}
```

### Macro Layer

```cpp
#define SPARK_LOG(level, category, fmt, ...) \
    do { \
        if (Spark::Logger::Get().ShouldLog(level, category)) \
            Spark::Logger::Get().Log(level, category, \
                std::source_location::current(), \
                std::format(fmt, ##__VA_ARGS__)); \
    } while(0)

// Convenience
#define SPARK_LOG_TRACE(cat, fmt, ...)  SPARK_LOG(Spark::LogLevel::Trace, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_DEBUG(cat, fmt, ...)  SPARK_LOG(Spark::LogLevel::Debug, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_INFO(cat, fmt, ...)   SPARK_LOG(Spark::LogLevel::Info,  cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_WARN(cat, fmt, ...)   SPARK_LOG(Spark::LogLevel::Warn,  cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_ERROR(cat, fmt, ...)  SPARK_LOG(Spark::LogLevel::Error, cat, fmt, ##__VA_ARGS__)
#define SPARK_LOG_FATAL(cat, fmt, ...)  SPARK_LOG(Spark::LogLevel::Fatal, cat, fmt, ##__VA_ARGS__)

// Conditional variants
#define SPARK_LOG_ONCE(level, cat, fmt, ...)      ...
#define SPARK_LOG_EVERY_N(level, cat, N, fmt, ...) ...
#define SPARK_LOG_IF(level, cat, cond, fmt, ...)   ...

// Compile-time stripping for Trace/Debug in Release
#ifdef NDEBUG
#undef SPARK_LOG_TRACE
#define SPARK_LOG_TRACE(cat, fmt, ...) ((void)0)
#endif
```

### Built-in Sinks

| Sink | Output | When |
|------|--------|------|
| `StderrSink` | `fprintf(stderr)` + `OutputDebugStringA` | Always |
| `FileSink` | Timestamped `.log` files with rotation | Always |
| `ConsoleSink` | `SimpleConsole::Log()` | When console is initialized |
| `EditorSink` | `EditorLogger` memory buffer | When editor is running |
| `CallbackSink` | User-registered `std::function` | For game/plugin integration |

---

## Where Logging Should Be Added (Per-Subsystem)

### Core / Engine Startup

- Engine initialization sequence (each subsystem init success/failure)
- `EngineContext` service registration and lookup failures
- Module loading (DLL load, symbol resolution, version checks)
- Configuration file parsing (loaded values, missing keys, defaults used)

### Graphics

- Replace all 177 `LOG_TO_CONSOLE` + 12 `printf` calls with `SPARK_LOG(*, LogCategory::Graphics, ...)`
- Add: shader compilation warnings (not just errors), pipeline state changes, resource creation/destruction counts per frame, GPU memory usage at startup

### Physics

- Rigidbody creation/destruction, collision pair events (debug level)
- Physics world step timing anomalies (>16ms)
- Constraint creation failures, invalid collision shapes
- Bullet Physics warning callback integration

### Audio

- XAudio2 device enumeration and selection
- Sound file load success/failure with file paths
- Voice pool exhaustion warnings
- 3D audio listener/emitter position updates (trace level)
- Music track transitions

### AI

- NavMesh bake start/complete with timing and triangle count
- Pathfinding requests and results (found/not-found, path length)
- Behavior tree node entry/exit at debug level
- Perception events (sight, sound) at trace level

### Animation

- Skeleton/animation clip load success/failure
- Missing bone warnings during retargeting
- State machine transitions
- IK solver convergence failures
- Blend tree evaluation at trace level

### Networking

- Connection state changes (connecting, connected, disconnected, error)
- Message send/receive counts per second (periodic)
- Deserialization errors with message type
- Bandwidth usage summaries
- Timeout and retry events

### ECS

- Entity creation/destruction counts (periodic summary, not per-entity)
- Component type registration
- System registration and execution order
- Archetype changes at debug level

### Scene Management

- Replace 18 `LOG_TO_CONSOLE` calls with `SPARK_LOG`
- Scene load start/complete with timing
- Entity count after load
- Asset reference resolution failures

### Save System

- Save/load start and completion with file path and timing
- Version mismatch warnings
- Corrupt data detection with byte offset
- Auto-save triggers

### Event System

- Handler registration/deregistration at debug level
- Unhandled event warnings
- Event queue overflow

### Input

- Device connection/disconnection
- Input mapping changes
- Input mode switches (keyboard/mouse to gamepad)

### Scripting

- Script compilation start/result with error messages
- Runtime script exceptions with stack trace
- API binding registration at debug level

### Cinematic

- Sequence play/stop/pause events
- Track binding failures
- Keyframe evaluation errors

### Procedural Generation

- Generation start/complete with seed and timing
- Chunk creation with parameters
- Generation failures or fallbacks

### Crash Handler

- Flush all log sinks before writing minidump
- Log the crash reason and faulting address
- Write final "crash occurred" entry to log file

---

## Migration Plan

### Phase 1: New Logger Implementation

1. Create `SparkEngine/Source/Utils/Logger.h` and `Logger.cpp` with unified `Spark::Logger`
2. Implement `ILogSink` interface and built-in sinks (Stderr, File, Console, Editor, Callback)
3. Define single `Spark::LogLevel` and `Spark::LogCategory` enums
4. Create new `SPARK_LOG_*` macros with `std::source_location` and `std::format`
5. Add conditional logging macros (`LOG_ONCE`, `LOG_EVERY_N`, `LOG_IF`)
6. Add runtime level filtering (global + per-category)
7. Add compile-time stripping for Trace/Debug in Release builds
8. Integrate with `SimpleConsole` as a sink (not a replacement — console keeps its command system)

### Phase 2: Migrate Existing Logging

1. Replace all 470+ `LOG_TO_CONSOLE*` calls with `SPARK_LOG_*`
2. Replace all 35 `SPARK_LOG_*` calls (old SparkError macros) with new macros
3. Replace all 246 `console.Log*()` direct calls with `SPARK_LOG_*`
4. Replace all 120+ raw `printf`/`fprintf`/`cout`/`cerr` calls with `SPARK_LOG_*`
5. Replace all 12 direct `OutputDebugStringA` calls with `SPARK_LOG_*`
6. Replace `SE_LOG_*` editor macros to route through unified logger
7. Delete `LogMacros.h`, `FileLogger.h` (old), update `SparkError.h` to use new logger

### Phase 3: Add Missing Logging

1. Add logging to all 12 zero-coverage subsystems listed above
2. Focus on initialization/shutdown, error paths, and state transitions first
3. Add trace-level logging for per-frame operations behind compile-time guards
4. Add periodic summary logging for high-frequency events (entity counts, draw calls, network stats)

### Phase 4: Integration and Polish

1. Hook crash handler to flush all sinks before minidump
2. Add profiler log integration (frame spike warnings)
3. Add log deduplication (suppress repeated identical messages)
4. Add console commands: `log_level <level>`, `log_category <cat> <level>`, `log_flush`, `log_export`
5. Cross-platform testing (Linux terminal output, macOS console)

---

## Summary Table

| # | Gap | Severity | Category |
|---|-----|----------|----------|
| 1 | Four disconnected logging systems | Critical | Architecture |
| 2 | Three duplicate severity enums | Critical | Architecture |
| 3 | FileLogger is dead code | Critical | Architecture |
| 4 | Wide-string conversion is broken | Critical | Correctness |
| 5 | No runtime log level filtering | Critical | Performance |
| 6 | 12/14 engine subsystems have zero logging | Critical | Coverage |
| 7 | Graphics uses LOG_TO_CONSOLE only | Critical | Coverage |
| 8 | Audio has 5 total log calls | Critical | Coverage |
| 9 | Core startup logging coupled to console init | Critical | Reliability |
| 10 | 120+ raw printf/cout calls bypass logging | Major | Coverage |
| 11 | 12 files use OutputDebugStringA directly | Major | Coverage |
| 12 | No structured log categories in engine | Major | Architecture |
| 13 | No conditional/periodic logging utilities | Major | Usability |
| 14 | Thread safety is inconsistent | Major | Correctness |
| 15 | SimpleConsole is Windows-only | Major | Platform |
| 16 | No log callback/event hook system | Moderate | Extensibility |
| 17 | No compile-time log stripping | Moderate | Performance |
| 18 | No std::format / C++20 integration | Moderate | Modernization |
| 19 | No log message deduplication | Moderate | Usability |
| 20 | Fixed-size message buffers (1-2KB) | Moderate | Correctness |
| 21 | Profiler has no log integration | Moderate | Observability |
| 22 | Crash handler doesn't flush logs | Moderate | Reliability |
