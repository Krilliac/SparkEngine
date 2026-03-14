# ConsoleProcessManager Wiring — Critical Issue

**Last updated:** 2026-03-14
**Type:** Issue
**Status:** Active
**Priority:** 🔴 CRITICAL

---

## Executive Summary

**ConsoleProcessManager is built but never initialized or called.** It's a complete system that sits idle, preventing SparkConsole.exe from launching and commands from being executed.

**Fix time:** ~15 minutes (3 lines of code)

---

## Problem

### Current State
- ✅ ConsoleProcessManager class exists and is fully implemented
- ✅ `Initialize()` method spawns SparkConsole.exe subprocess
- ✅ `ProcessCommands()` method reads from subprocess pipe and executes commands
- ❌ `Initialize()` is **never called** during engine startup
- ❌ `ProcessCommands()` is **never called** in the main loop

**Result:** SparkConsole.exe never launches. Commands are silently dropped.

### Evidence

**SparkEngine.cpp — startup path:**
```cpp
// Line 294: Headless mode
if (console.Initialize())  // SimpleConsole, NOT ConsoleProcessManager
{
    g_moduleManager->InitializeAll(EngineContext::Get());
}

// Line 635: Normal startup
if (console.Initialize())  // Again, SimpleConsole only
{
    g_input->Initialize(hWnd);
}

// Line 1661: Play mode
if (console.Initialize())  // Again, SimpleConsole only
{
    // ...
}

// NO ConsoleProcessManager::GetInstance().Initialize() anywhere
```

**SparkEngine.cpp — main loop:**
```cpp
// Main game loop (approx lines 1100–1300)
while (!shutdownRequested)
{
    // Update graphics, input, physics, AI, audio...

    // NO ConsoleProcessManager::GetInstance().ProcessCommands() call
}
```

**Result:** SparkConsole.exe subprocess never launches, command pipe never opens, commands are dropped.

---

## Architecture

### How It Should Work

```
1. Engine startup
   └─ ConsoleProcessManager::GetInstance().Initialize()
       └─ Spawns SparkConsole.exe subprocess
       └─ Opens pipe for IPC

2. Main loop (every frame)
   └─ ConsoleProcessManager::GetInstance().ProcessCommands()
       ├─ Read from pipe (non-blocking)
       ├─ If command received: execute via SimpleConsole::ExecuteCommand()
       └─ Send response back through pipe

3. Engine shutdown
   └─ ConsoleProcessManager::GetInstance().Shutdown()
       └─ Closes pipe, terminates subprocess
```

### Current Broken State

```
1. Engine startup
   └─ SimpleConsole::Initialize() ✅ (but ProcessManager never initialized)

2. Main loop
   └─ [NO ProcessCommands call] ❌

3. Engine shutdown
   └─ [Subprocess left running or crashes]
```

---

## Fix (3 lines of code)

### Step 1: Initialize ConsoleProcessManager at Startup

**File:** `SparkEngine/Source/Core/SparkEngine.cpp`
**Location:** After SimpleConsole initialization, around line 640

```cpp
// Existing code
auto& console = Spark::SimpleConsole::GetInstance();
if (console.Initialize())
{
    console.LogSuccess("Console initialized.");

    // ADD THESE 2 LINES:
    Spark::ConsoleProcessManager::GetInstance().Initialize();
    console.LogSuccess("ConsoleProcessManager spawned subprocess.");
}
```

### Step 2: Call ProcessCommands in Main Loop

**File:** `SparkEngine/Source/Core/SparkEngine.cpp`
**Location:** Main game loop, after all engine updates, around line 1200

```cpp
// After physics, graphics, AI, audio updates...

// ADD THIS 1 LINE:
Spark::ConsoleProcessManager::GetInstance().ProcessCommands();

// Then present/swap buffers
```

### Step 3: Verify in Shutdown

**File:** `SparkEngine/Source/Core/SparkEngine.cpp`
**Location:** Cleanup section (should already exist)

```cpp
// Should already have:
Spark::ConsoleProcessManager::GetInstance().Shutdown();
```

---

## Testing Checklist

### Pre-Wiring
```
// Verify the subprocess is NOT running
ps aux | grep SparkConsole.exe  // Should NOT appear
```

### Post-Wiring
1. Launch SparkEngine.exe
2. Verify SparkConsole.exe appears in process list
3. Send a command from console: `help`
4. Verify it's handled (response appears in console)
5. Send: `sys_info` (engine info command)
6. Verify metrics appear
7. Kill engine; verify SparkConsole.exe also terminates

### CI/Tests
- [ ] Engine compiles with changes
- [ ] `ctest` passes
- [ ] clang-format passes
- [ ] No warnings

---

## Why This Matters

### Current Impact
- **Commands never execute** (UI sends command, nothing happens)
- **Game logic depending on console can't work** (e.g., admin tools, debugging)
- **Performance profiling via console is broken**
- **Crash logs sent to ConsoleProcessManager go nowhere** (line 6 in CrashHandler.cpp)

### After Fix
- ✅ Commands route from subprocess to engine and back
- ✅ Real-time game state inspection (FPS, memory, entity count)
- ✅ Debug tools become functional
- ✅ Crash logs captured and stored

---

## Related Code

### ConsoleProcessManager Interface

```cpp
// SparkEngine/Source/Utils/ConsoleProcessManager.h
namespace Spark
{
    class ConsoleProcessManager
    {
      public:
        static ConsoleProcessManager& GetInstance();

        bool Initialize(const std::wstring& consolePath = L"SparkConsole");
        void Shutdown();
        void ProcessCommands();
        void Log(const std::wstring& message, const std::wstring& type = L"INFO");
        void LogCrash(const std::string& crashInfo);
    };
}
```

**Key methods:**
- `Initialize()` → Spawns SparkConsole.exe, opens pipe
- `ProcessCommands()` → Polls pipe for new commands, executes them
- `Shutdown()` → Closes pipe, terminates subprocess
- `Log()` / `LogCrash()` → Send messages to subprocess

### Usage Pattern

```cpp
// Startup
Spark::ConsoleProcessManager& mgr = Spark::ConsoleProcessManager::GetInstance();
mgr.Initialize();

// Main loop
while (running) {
    // ... engine updates ...
    mgr.ProcessCommands();  // Once per frame
}

// Shutdown
mgr.Shutdown();
```

---

## Known Issues

### Issue 1: SimpleConsole::Initialize() Called Multiple Times

**Current:** Lines 294, 635, 1661 in SparkEngine.cpp all call SimpleConsole::Initialize().

**Expected:** Call it once at startup.

**Bloat issue:** See `.claude/knowledge/codebase-bloat-audit-2026-03-14.md`

**Action:** Consolidate initialization in next refactor (sparkconsole-refactor-plan.md).

### Issue 2: Missing Includes

Verify that `#include "Utils/ConsoleProcessManager.h"` exists at the top of SparkEngine.cpp. If not, add it.

---

## Commit Message

```
refactor: wire ConsoleProcessManager into engine startup and main loop

- Initialize ConsoleProcessManager in startup (spawns SparkConsole.exe)
- Call ProcessCommands() each frame in main loop
- Enables console command execution and real-time debugging
- Fixes: commands route from subprocess to engine and back
```

---

## Success Criteria

✅ SparkConsole.exe appears in process list when engine is running
✅ Commands sent from console are received and executed in engine
✅ Engine info commands return live data
✅ Shutdown terminates console subprocess cleanly
✅ All unit tests pass
✅ CI green

---

## Follow-Up Tasks

1. **Consolidate SimpleConsole initialization** (see sparkconsole-refactor-plan.md)
2. **Delete embedded UI code from SparkConsole** (6,000+ lines of bloat)
3. **Extract command registrations to ConsoleCommandRegistry** (52 Register* methods)
4. **Add unit test for ConsoleProcessManager** (ensure subprocess lifecycle is correct)

---

## References

- `.claude/knowledge/codebase-bloat-audit-2026-03-14.md` — Full audit identifying this issue
- `.claude/knowledge/sparkconsole-refactor-plan.md` — Detailed refactor plan for SparkConsole
- `CLAUDE.md` section "Wiring Things In" — General principle that applies here
- `SparkEngine/Source/Utils/ConsoleProcessManager.{h,cpp}` — Implementation
- `SparkEngine/Source/Core/SparkEngine.cpp` — Where to add initialization and loop call
