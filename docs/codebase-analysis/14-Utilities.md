# 14 — Utilities

**Location:** `SparkEngine/Source/Utils/`

Foundational utilities used throughout the engine: logging, console, profiling, assertions, validation, math, file I/O, crash handling, CVars, and more.

---

## Logger — Structured Async Logging

**File:** `SparkEngine/Source/Utils/Logger.h`

```cpp
// Macros (primary usage)
SPARK_LOG_INFO("Player spawned at ({}, {}, {})", x, y, z);
SPARK_LOG_WARN("Low memory: {} MB remaining", freeMB);
SPARK_LOG_ERROR("Failed to load texture: {}", path);
```

### Severity Levels
`Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`, `Off`

### Categories
`Core`, `Graphics`, `Physics`, `Audio`, `AI`, `Animation`, `ECS`, `Network`, `Input`, `Scripting`, `Scene`, `Save`, `Cinematic`, `Procedural`, `Editor`, `Game`

### Sink Architecture

```cpp
Logger::GetInstance().AddSink(std::make_unique<StderrSink>());
Logger::GetInstance().AddSink(std::make_unique<FileSink>("Logs/engine.log", 50*MB, 5)); // 50MB rotation, 5 backups
Logger::GetInstance().AddSink(std::make_unique<CallbackSink>([](const LogMessage& msg) {
    // Custom handling
}));
```

### Thread Safety
Async writer thread with message queue — logging never blocks the main loop.

---

## SimpleConsole — Command Registry

**File:** `SparkEngine/Source/Utils/SparkConsole.h`

```cpp
auto& console = SimpleConsole::GetInstance();

console.RegisterCommand("god", [](const auto& args) {
    ToggleGodMode();
}, "Toggle god mode", "Cheat", "god", Permission::Developer);

console.ExecuteCommand("god");
console.Log("Player health: 100", LogType::Info);
```

### Permission Levels
`Player`, `Moderator`, `Admin`, `Developer`

### Features
- Command aliases (`q` → `quit`)
- History tracking (2000 logs, 500 commands)
- Thread-safe (mutex-protected)

---

## ConsoleProcessManager — External Console IPC

**File:** `SparkEngine/Source/Utils/ConsoleProcessManager.h`

```cpp
auto& cpm = ConsoleProcessManager::GetInstance();
cpm.Initialize("SparkConsole.exe");

// Per-frame
cpm.Log("Frame time: 16.3ms", LogType::Info);  // Send to child
cpm.ProcessCommands();                            // Read from child
cpm.Shutdown();
```

- **Windows**: CreateProcess + Win32 pipes
- **Linux**: fork/exec + POSIX pipes
- Background reader thread prevents main-loop blocking

---

## Assert — Crash-Aware Assertions

**File:** `SparkEngine/Source/Utils/Assert.h`

```cpp
ASSERT(ptr != nullptr);
ASSERT_MSG(count > 0, "Count must be positive");
ASSERT_NOT_NULL(texture);
ASSERT_IN_RANGE(index, 0, arraySize);
ASSERT_HR(device->CreateBuffer(&desc, nullptr, &buffer));

// Always-on (not stripped in release)
ASSERT_ALWAYS(criticalCondition);
VERIFY(ImportantOperation());
VERIFY_HR(d3dCall);
```

On failure: logs file:line, timestamp, thread ID, stack trace, then calls `TriggerCrashHandler()` before aborting.

---

## Validate — Defensive & Offensive Validation

**File:** `SparkEngine/Source/Utils/Validate.h`

```cpp
// Defensive (log + return)
SPARK_VALIDATE(ptr != nullptr);              // Returns if false
SPARK_VALIDATE_NOT_NULL(texture);
SPARK_VALIDATE_RANGE(index, 0, size);
SPARK_VALIDATE_NOT_EMPTY(container);

// Offensive (abort on failure)
SPARK_REQUIRE(count > 0);                    // Precondition
SPARK_ENSURE(result.IsOk());                 // Postcondition
SPARK_UNREACHABLE();                          // Should never execute

// Diagnostics
SPARK_TRACE_ENTER();                          // Function entry
SPARK_TRACE_EXIT();                           // Function exit
SPARK_TRACE_SCOPE("LoadTexture");             // RAII scope
SPARK_DIAGNOSTIC_CONTEXT("Loading level 3");  // Crash breadcrumb
```

---

## Profiler — Frame Performance Analysis

**File:** `SparkEngine/Source/Utils/Profiler.h`

```cpp
{
    ScopedProfileTimer timer(ProfileCategory::Render);
    // Rendering code...
}

// Access metrics
auto& profiler = Profiler::GetInstance();
float frameTime = profiler.GetAverageFrameTime();
float renderTime = profiler.GetCategoryTime(ProfileCategory::Render);
```

Categories: `Frame`, `Render`, `Physics`, `Audio`, `GameLogic`, `Input`, `Particles`, `UI`, `Custom`

300-sample ring buffer (5 seconds at 60 FPS). GPU timing queries on Windows D3D11.

---

## ConsoleVariable (CVar) — Typed Runtime Variables

**File:** `SparkEngine/Source/Utils/ConsoleVariable.h`

```cpp
static CVar<float> cv_gravity("physics.gravity", -9.81f,
    CVarFlags::Save, "World gravity", -100.0f, 0.0f);

static CVar<bool> cv_showFPS("debug.showFPS", false,
    CVarFlags::None, "Show FPS counter");

static CVar<int> cv_msaa("graphics.msaa", 4,
    CVarFlags::RequiresRestart, "MSAA sample count", 1, 8);
```

Each CVar auto-generates console commands:
- `physics.gravity` → query current value
- `physics.gravity -20.0` → set new value

Flags: `ReadOnly`, `Save`, `Cheat`, `Hidden`, `RequiresRestart`

Built-in commands: `cvar_list`, `cvar_reset_all`

---

## Timer — High-Precision Frame Timing

**File:** `SparkEngine/Source/Utils/Timer.h`

```cpp
Timer timer;
timer.Start();

// Each frame
float dt = timer.GetDeltaTime();
float total = timer.GetTotalTime();

timer.Stop();   // Pause
timer.Start();  // Resume
timer.Reset();  // Zero
```

Backed by `std::chrono::high_resolution_clock`.

---

## MathUtils — Mathematical Utilities

**Files:** `SparkEngine/Source/Utils/MathUtils.h`, `MathUtilsExtended.h`

```cpp
// Constants
float pi = MathUtils::PI;
float deg90 = MathUtils::HALF_PI;

// Conversions
float rad = MathUtils::DegreesToRadians(45.0f);
float deg = MathUtils::RadiansToDegrees(MathUtils::PI);

// Interpolation
float val = MathUtils::Lerp(0.0f, 100.0f, 0.5f);     // 50.0
float smooth = MathUtils::SmoothStep(0.0f, 1.0f, t);

// Vector operations
float dist = MathUtils::Distance(pointA, pointB);
XMFLOAT3 dir = MathUtils::Normalize(direction);
float dot = MathUtils::Dot(a, b);
XMFLOAT3 cross = MathUtils::Cross(a, b);

// Extended
float random = MathUtilsExtended::RandomFloat(0.0f, 1.0f);
bool inSphere = MathUtilsExtended::PointInSphere(point, center, radius);
float eased = MathUtilsExtended::EaseInOutCubic(t);
```

---

## FileUtils — Cross-Platform File I/O

**File:** `SparkEngine/Source/Utils/FileUtils.h`

```cpp
auto text = FileUtils::ReadTextFile("Data/Config/settings.ini");
if (text) ProcessConfig(*text);

FileUtils::WriteTextFile("Data/Saves/save.json", jsonString);

auto bytes = FileUtils::ReadBinaryFile("Data/Meshes/model.mesh");
FileUtils::WriteBinaryFile("Data/Cache/compiled.bin", data);

bool exists = FileUtils::FileExists("Data/Textures/brick.png");
auto files = FileUtils::ListFilesRecursive("Data/Scripts/", ".as");
std::string dir = FileUtils::GetDirectory("/path/to/file.txt");
std::string joined = FileUtils::JoinPath("Data", "Textures", "brick.png");
```

---

## CrashHandler — Crash Reporting

**File:** `SparkEngine/Source/Utils/CrashHandler.h`

```cpp
CrashConfig config;
config.minidumpPrefix = "SparkEngine_";
config.captureScreenshot = true;
config.captureSystemInfo = true;
config.compressBeforeUpload = true;
config.uploadUrl = "https://crashes.example.com/submit";

InstallCrashHandler(config);
```

- **Windows**: SEH unhandled-exception filter + minidump generation
- **Linux**: Signal handlers (SIGSEGV, SIGABRT, etc.)
- Collects: minidump, thread stacks, system info, screenshot
- Optional GitHub issue auto-creation

---

## Other Utilities

| Utility | File | Purpose |
|---------|------|---------|
| `Hash` | `Hash.h` | FNV-1a compile-time & runtime hashing (`"key"_hash64`) |
| `Result<T>` | `Result.h` | Type-safe error handling (Ok/Err, no exceptions) |
| `ScopeGuard` | `ScopeGuard.h` | RAII cleanup (ScopeExit, ScopeSuccess, ScopeFail) |
| `ThreadSafeQueue` | `ThreadSafeQueue.h` | Mutex-guarded FIFO with optional capacity |
| `StringUtils` | `StringUtils.h` | ToLower, Trim, Split, Join, Contains, Replace |
| `ConfigParser` | `ConfigParser.h` | INI file read/write |
| `UUID` | `UUID.h` | Unique ID generation |
| `Cooldown` | `Cooldown.h` | Reusable cooldown timer |
| `DeltaSmoother` | `DeltaSmoother.h` | Smooth delta time (prevent jitter) |
| `ScopedTimer` | `ScopedTimer.h` | RAII timer for profiling blocks |
| `TypeTraits` | `TypeTraits.h` | C++23 type introspection helpers |
| `MultiISA` | `MultiISA.h` | SIMD codepath selection (AVX2, SSE, scalar) |
| `ColorUtils` | `ColorUtils.h` | Color space conversions (sRGB, linear, HSV) |
| `BitFlags` | `BitFlags.h` | Strongly-typed bitset wrappers |
| `FrameAllocator` | `FrameAllocator.h` | Per-frame temporary allocations |
| `RingBuffer` | `RingBuffer.h` | Circular buffer for streaming/networking |
| `LocalFileCache` | `LocalFileCache.h` | In-memory file cache |
| `MemoryDebugger` | `MemoryDebugger.h` | Memory profiling and leak detection |
| `EventBus` | `EventBus.h` | Global pub/sub event dispatch |
| `EntityEventBus` | `EntityEventBus.h` | Entity-specific event routing |
| `DebugDraw` | `DebugDraw.h` | Wireframe rendering overlay |
| `DebugOverlay` | `DebugOverlay.h` | ImGui-based debug panels |
| `ChromeTracing` | `ChromeTracing.h` | Trace export for chrome://tracing |
| `SparkError` | `SparkError.h` | Error types and codes |
