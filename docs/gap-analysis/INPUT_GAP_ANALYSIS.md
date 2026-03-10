# SparkEngine Input — Gap Analysis

> **Scope**: `SparkEngine/Source/Input/` (InputManager, GamepadInput, PlatformInput)
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Input/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Input subsystem has three layers: a low-level `InputManager` (Win32-only keyboard/mouse), a `GamepadInput` class (XInput-only gamepad), and a higher-level `PlatformInputManager` with a pluggable backend architecture (`IPlatformInputBackend`). The platform abstraction layer (`PlatformInput.h`) is well-designed with unified `KeyCode`/`GamepadBtn` enums, action/axis mapping, and an SDL2 backend stub. However, the legacy `InputManager` class is still the one registered in `EngineContext`, and the two systems are not integrated.

---

## Critical Gaps

### GAP-I01 — Two Competing Input Systems, Neither Fully Integrated

**Files**:
- `Input/InputManager.h` — Legacy system registered in `EngineContext`
- `Input/PlatformInput.h` — Modern abstraction, not registered in `EngineContext`

**Impact**: The engine has two parallel input systems that duplicate functionality. `InputManager` is the one exposed through `EngineContext::GetInput()`, but it uses raw Win32 `HWND`, `UINT`, `WPARAM`, and `LPARAM` types throughout its public API, making it unusable on non-Windows platforms. `PlatformInputManager` was designed to replace it with cross-platform backends, but it exists as a separate singleton and is not wired into the engine context or game module interface.

**Evidence**: `EngineContext.h` forward-declares and stores `InputManager*`, not `PlatformInputManager*`. `InputManager::HandleMessage()` takes `UINT message, WPARAM wParam, LPARAM lParam` — Win32 types. `PlatformInputManager` takes `void* windowHandle` and has its own `HandleWindowMessage()` with platform-neutral `uint32_t`/`uintptr_t` parameter types.

**What is needed**: Migrate `EngineContext` to expose `PlatformInputManager` (or `IPlatformInputBackend`) instead of `InputManager`. Deprecate `InputManager` and route all game code through the platform-agnostic layer. Update `IEngineContext` interface to use `PlatformInputManager*`.

---

### GAP-I02 — SDL2 Backend Compile-Gated and Likely Untested

**Files**:
- `Input/PlatformInput.h` (`SDL2InputBackend` class, lines 516–563)

**Impact**: The SDL2 backend (the only cross-platform backend) is gated behind `#ifdef SPARK_SDL2_AVAILABLE`, which is not defined anywhere in the CMake build system. SDL2 is not listed as a dependency. The backend class is declared but there is no evidence of implementation or testing. Linux and macOS builds therefore have no functional input backend.

**Evidence**: `SPARK_SDL2_AVAILABLE` does not appear in any `CMakeLists.txt`. No SDL2 find module or dependency is present. The class body is header-only declarations with no corresponding `.cpp` for SDL2-specific logic.

**What is needed**: Either integrate SDL2 as an optional dependency with a CMake find module and `ENABLE_SDL2` toggle, or implement a Linux evdev/libinput backend. At minimum, provide a null input backend that compiles and returns safe defaults.

---

## Major Gaps

### GAP-I03 — InputManager Key Binding System Is Keyboard-Only

**Files**:
- `Input/InputManager.h` (lines 72–74, `m_keyBindings`)

**Impact**: `InputManager` has a key binding system (`Console_BindKey`, `Console_IsActionActive`) but it only maps action names to integer virtual key codes. It cannot bind gamepad buttons, mouse buttons, or analog axes to actions. The `GamepadInput` class has its own separate `BindAction()` system that is not connected to `InputManager`'s bindings.

**Evidence**: `m_keyBindings` is `std::unordered_map<std::string, int>` — maps action names to Win32 virtual key codes only. `GamepadInput::BindAction()` is a separate API with its own `ActionBinding` struct.

**What is needed**: `PlatformInputManager` already solves this with unified `BindAction(name, KeyCode)` and `BindAction(name, GamepadBtn)`. Complete the migration to `PlatformInputManager` to get unified input bindings across all devices.

---

### GAP-I04 — No Touch or Gesture Input Support

**Files**: All input files

**Impact**: No support for touch screens, multi-touch gestures, or stylus input. While the engine targets desktop FPS games primarily, touch support is necessary for any potential mobile or tablet builds, and increasingly relevant for touch-enabled laptops.

**What is needed**: Add `TouchEvent` types to the `InputEvent` struct in `PlatformInput.h` (touch begin/move/end with finger ID and position). Implement touch handling in the SDL2 backend (SDL2 has built-in touch support).

---

### GAP-I05 — No Input Serialization or Config File Persistence

**Files**: `Input/InputManager.h`, `Input/PlatformInput.h`

**Impact**: Neither input system can save or load key bindings to/from a configuration file. Players cannot persist their custom key bindings between sessions. The `Console_BindKey()` changes are lost on restart.

**What is needed**: Add `SaveBindings(filepath)` and `LoadBindings(filepath)` methods to `PlatformInputManager`. Use a simple JSON or INI format. Load bindings at startup and save when modified.

---

## Moderate Gaps

### GAP-I06 — HandleMessage Thread Safety Concern

**Files**:
- `Input/InputManager.h` (line 81, `m_inputMutex`; line 131, `HandleMessage`)

**Impact**: `HandleMessage()` is called from the Win32 `WndProc` callback, which runs on the window's message pump thread. Meanwhile, `Update()` and query methods run on the main game thread. While `m_inputMutex` exists, it is unclear whether `HandleMessage()` acquires it before modifying `m_keyStates` and mouse position. If not, concurrent reads and writes cause data races.

**What is needed**: Verify `.cpp` implementation acquires the mutex in `HandleMessage()`. Alternatively, use a double-buffer pattern: `HandleMessage()` writes to a pending buffer, `Update()` swaps buffers under the lock.

---

### GAP-I07 — Console_SimulateKeyPress Uses Threads for Timed Release

**Files**:
- `Input/InputManager.h` (lines 83, 344)

**Impact**: `Console_SimulateKeyPress(keyName, duration)` spawns a `std::thread` to sleep for `duration` milliseconds then release the key. These threads are stored in `m_pendingTimedThreads` and joined in the destructor. This pattern is fragile: if many simulated presses are requested, many threads are spawned. If the `InputManager` is destroyed while threads are sleeping, the destructor blocks.

**What is needed**: Use the `CoroutineScheduler` or a timer callback instead of spawning OS threads. Alternatively, use `WaitForSeconds` from the coroutine system to defer the key release.

---

### GAP-I08 — No Input Buffering for Frame-Perfect Actions

**Files**: All input files

**Impact**: Input is sampled once per frame in `Update()`. If a key press and release occur between two `Update()` calls (common at low frame rates), the press is lost. For an FPS game, this means fast button presses (e.g., quick melee, weapon switch) can be dropped.

**What is needed**: Buffer input events as they arrive in `HandleMessage()` and process the full buffer in `Update()`. Track "pressed this frame" as "was pressed at any point since last Update" rather than instantaneous state.

---

## Minor Gaps

### GAP-I09 — Mouse Wheel/Scroll Not Handled in InputManager

**Files**:
- `Input/InputManager.h` (line 131, `HandleMessage` — lists WM_MOUSEMOVE but not WM_MOUSEWHEEL)

**Impact**: The `InputManager::HandleMessage()` documentation lists the handled messages and does not include `WM_MOUSEWHEEL`. Scroll wheel input may be silently ignored. The `PlatformInputManager`'s `Win32InputBackend` does handle `WM_MOUSEWHEEL` via `GetMouseScroll()`, so this only affects code using the legacy `InputManager`.

**What is needed**: Add `WM_MOUSEWHEEL` handling to `InputManager::HandleMessage()` or complete migration to `PlatformInputManager` which already supports it.

---

### GAP-I10 — No Input Recording/Playback for Debugging

**Files**: All input files

**Impact**: No facility to record input sequences and replay them. This would be valuable for automated testing, bug reproduction, and demo recording. `m_recentInputEvents` stores recent events for console display but cannot replay them.

**What is needed**: Add optional input recording that serializes timestamped events to a file, and a playback mode that feeds recorded events back into the system. Useful for automated testing.

---
