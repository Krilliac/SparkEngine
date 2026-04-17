# Platform Input

The Platform Input system provides a cross-platform input abstraction that decouples game logic from OS-specific input APIs. It features unified key codes for keyboard, mouse, and gamepad; named action mapping with primary/secondary bindings; pluggable backends (Win32, SDL2, Null) via a factory; analog input with configurable deadzones; and JSON serialization for user key remapping. It lives in the `Spark::Input` namespace.

**Source:** `SparkEngine/Source/Input/PlatformInput.h`

## Overview

| Class / Struct | Responsibility |
|---|---|
| `InputActionMap` | Maps named actions to physical key bindings with frame-edge detection (just pressed, just released) |
| `IPlatformInputBackend` | Abstract interface for platform-specific input backends |
| `NullInputBackend` | No-op backend for headless mode and unit tests |
| `PlatformInputFactory` | Factory that creates the appropriate backend for the current platform |
| `InputAction` | A named input action bound to one or two physical keys/buttons with analog support |
| `PlatformKeyCode` | Unified key/button code enum covering keyboard, mouse, and gamepad |

## Key Enums and Types

### PlatformKeyCode

Unified key/button codes across all input devices. Values are engine-defined and do not map 1:1 to OS virtual key codes -- backends translate native codes to and from `PlatformKeyCode`.

```cpp
enum class PlatformKeyCode : uint16_t
{
    None = 0,

    // Letters: A=1 through Z=26
    A = 1, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Digits: Num0=30 through Num9=39
    Num0 = 30, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,

    // Function keys: F1=50 through F12=61
    F1 = 50, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // Navigation
    Space = 70, Enter, Escape, Tab, Backspace, Delete,
    Home, End, PageUp, PageDown, Left, Right, Up, Down,

    // Modifiers
    Shift = 90, Ctrl, Alt,

    // Mouse buttons
    Mouse_Left = 110, Mouse_Right, Mouse_Middle, Mouse_X1, Mouse_X2,

    // Gamepad
    Gamepad_A = 130, Gamepad_B, Gamepad_X, Gamepad_Y,
    Gamepad_LB, Gamepad_RB, Gamepad_LT, Gamepad_RT,
    Gamepad_Start, Gamepad_Back,
    Gamepad_LeftStick, Gamepad_RightStick,
    Gamepad_DPadUp, Gamepad_DPadDown, Gamepad_DPadLeft, Gamepad_DPadRight,
    Gamepad_LeftStickX, Gamepad_LeftStickY,
    Gamepad_RightStickX, Gamepad_RightStickY,

    Count
};
```

### InputAction

```cpp
struct InputAction
{
    std::string name;                                     // Human-readable action name
    PlatformKeyCode primaryKey = PlatformKeyCode::None;   // Primary binding
    PlatformKeyCode secondaryKey = PlatformKeyCode::None; // Alternate binding
    float deadzone = 0.15f;                               // Analog deadzone (0-1)
    bool isPressed = false;                               // Currently held
    bool isJustPressed = false;                           // Pressed this frame
    bool isJustReleased = false;                          // Released this frame
    float analogValue = 0.0f;                             // Analog axis value (-1 to 1)
};
```

## Quick Start

### Creating a backend and registering actions

```cpp
#include "Input/PlatformInput.h"

// Create platform-appropriate backend
auto backend = Spark::Input::PlatformInputFactory::CreateBackend();
backend->Initialize();

// Set up action map
Spark::Input::InputActionMap actions;
actions.RegisterAction("Jump",        Spark::Input::PlatformKeyCode::Space);
actions.RegisterAction("Fire",        Spark::Input::PlatformKeyCode::Mouse_Left);
actions.RegisterAction("MoveForward", Spark::Input::PlatformKeyCode::W,
                                      Spark::Input::PlatformKeyCode::Up); // secondary binding
actions.RegisterAction("MoveRight",   Spark::Input::PlatformKeyCode::D,
                                      Spark::Input::PlatformKeyCode::Right);
```

### Per-frame input polling

```cpp
void GameLoop()
{
    // Step 1: Poll OS events
    backend->PollEvents();

    // Step 2: Update action states from backend
    actions.UpdateActions(*backend);

    // Step 3: Query actions
    if (actions.IsActionJustPressed("Jump"))
    {
        player.Jump();
    }

    if (actions.IsActionPressed("Fire"))
    {
        weapon.Fire();
    }

    float moveX = actions.GetActionAnalog("MoveRight");
    float moveY = actions.GetActionAnalog("MoveForward");
    player.Move(moveX, moveY);
}
```

### Gamepad input with deadzones

```cpp
// Register stick-based movement with a custom deadzone
Spark::Input::InputAction lookAction;
lookAction.name = "LookX";
lookAction.primaryKey = Spark::Input::PlatformKeyCode::Gamepad_RightStickX;
lookAction.deadzone = 0.2f; // 20% deadzone

// Or via RegisterAction and modify afterwards:
actions.RegisterAction("LookX", Spark::Input::PlatformKeyCode::Gamepad_RightStickX);

// Analog values range from -1 to 1 for axes, 0 to 1 for triggers
float lookSpeed = actions.GetActionAnalog("LookX");
camera.RotateYaw(lookSpeed * dt * sensitivity);
```

### Mouse input

```cpp
float mx, my;
backend->GetMousePosition(mx, my);

float dx, dy;
backend->GetMouseDelta(dx, dy);
camera.RotateByMouse(dx, dy);

// Hide cursor for FPS camera
backend->SetCursorVisible(false);
```

### Checking individual keys directly

```cpp
// Bypass action map for one-off queries
if (backend->IsKeyDown(Spark::Input::PlatformKeyCode::Escape))
{
    OpenPauseMenu();
}

float triggerValue = backend->GetAnalogValue(Spark::Input::PlatformKeyCode::Gamepad_RT);
if (triggerValue > 0.5f)
{
    vehicle.Accelerate(triggerValue);
}
```

## Saving and Loading Bindings

The `InputActionMap` supports JSON serialization for persisting user key remappings.

### Saving bindings to JSON

```cpp
std::string json = actions.SaveToConfig();
// Write json to a file, e.g., "Config/input.json"

// Output format:
// {
//   "Jump": { "primary": 70, "secondary": 0, "deadzone": 0.150000 },
//   "Fire": { "primary": 110, "secondary": 0, "deadzone": 0.150000 },
//   "MoveForward": { "primary": 23, "secondary": 83, "deadzone": 0.150000 }
// }
```

### Loading bindings from JSON

```cpp
std::string json = ReadFile("Config/input.json");
actions.LoadFromConfig(json);
// Existing actions not in the JSON are preserved
// Actions in the JSON are updated with new bindings
```

### Full save/load cycle

```cpp
// At game start:
Spark::Input::InputActionMap actions;
actions.RegisterAction("Jump", Spark::Input::PlatformKeyCode::Space);
actions.RegisterAction("Fire", Spark::Input::PlatformKeyCode::Mouse_Left);

// Load user overrides (if file exists)
if (auto json = TryReadFile("Config/input.json"); !json.empty())
{
    actions.LoadFromConfig(json);
}

// ... gameplay with user's custom bindings ...

// At shutdown or when user changes bindings:
std::string json = actions.SaveToConfig();
WriteFile("Config/input.json", json);
```

## Writing Custom Backends

Implement `IPlatformInputBackend` to support new input APIs:

```cpp
class SDL2InputBackend final : public Spark::Input::IPlatformInputBackend
{
public:
    bool Initialize() override
    {
        SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
        m_keyStates.fill(false);
        return true;
    }

    void Shutdown() override
    {
        SDL_Quit();
    }

    void PollEvents() override
    {
        m_prevKeyStates = m_keyStates;
        m_mouseDeltaX = 0.0f;
        m_mouseDeltaY = 0.0f;

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_KEYDOWN:
                m_keyStates[TranslateSDLKey(event.key.keysym.sym)] = true;
                break;
            case SDL_KEYUP:
                m_keyStates[TranslateSDLKey(event.key.keysym.sym)] = false;
                break;
            case SDL_MOUSEMOTION:
                m_mouseDeltaX += static_cast<float>(event.motion.xrel);
                m_mouseDeltaY += static_cast<float>(event.motion.yrel);
                m_mouseX = static_cast<float>(event.motion.x);
                m_mouseY = static_cast<float>(event.motion.y);
                break;
            }
        }
    }

    bool IsKeyDown(Spark::Input::PlatformKeyCode key) const override
    {
        auto idx = static_cast<uint16_t>(key);
        return idx < m_keyStates.size() && m_keyStates[idx];
    }

    float GetAnalogValue(Spark::Input::PlatformKeyCode key) const override
    {
        // For digital keys, return 0 or 1
        return IsKeyDown(key) ? 1.0f : 0.0f;
    }

    void GetMousePosition(float& x, float& y) const override
    {
        x = m_mouseX;
        y = m_mouseY;
    }

    void GetMouseDelta(float& dx, float& dy) const override
    {
        dx = m_mouseDeltaX;
        dy = m_mouseDeltaY;
    }

    void SetCursorVisible(bool visible) override
    {
        SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
    }

    std::string_view GetBackendName() const override { return "SDL2"; }

private:
    // TranslateSDLKey maps SDL_Keycode -> PlatformKeyCode
    static size_t TranslateSDLKey(SDL_Keycode sdlKey);

    std::array<bool, static_cast<size_t>(Spark::Input::PlatformKeyCode::Count)> m_keyStates{};
    std::array<bool, static_cast<size_t>(Spark::Input::PlatformKeyCode::Count)> m_prevKeyStates{};
    float m_mouseX = 0.0f, m_mouseY = 0.0f;
    float m_mouseDeltaX = 0.0f, m_mouseDeltaY = 0.0f;
};
```

## NullInputBackend

The `NullInputBackend` is a no-op implementation used for headless mode (e.g., dedicated servers) and unit tests. It returns `false` / `0.0f` for all queries and does nothing on `PollEvents()`.

```cpp
// Automatically selected by PlatformInputFactory when no native backend is compiled in
auto backend = Spark::Input::PlatformInputFactory::CreateBackend();
// backend->GetBackendName() returns "Null"
```

## Console Commands

The input system does not currently expose dedicated console commands. Query state programmatically:

```cpp
// List all registered actions
for (const auto& [name, action] : actions.GetAllActions())
{
    Log::Info("Input", "Action '{}': primary={}, secondary={}, deadzone={:.2f}",
              name,
              static_cast<uint16_t>(action.primaryKey),
              static_cast<uint16_t>(action.secondaryKey),
              action.deadzone);
}
```

## Integration

### With EngineContext

Create the backend at engine startup and store it for the session:

```cpp
auto backend = Spark::Input::PlatformInputFactory::CreateBackend();
backend->Initialize();
// Store backend in EngineContext or as a member of the engine
```

### With the Accessibility System

Respect one-handed mode and input remapping settings:

```cpp
using namespace Spark::Accessibility;
const auto& settings = AccessibilitySystem::GetInstance().GetSettings();

if (settings.oneHandedMode == OneHandedMode::LeftHanded)
{
    actions.RegisterAction("Fire", PlatformKeyCode::Q);
    actions.RegisterAction("AltFire", PlatformKeyCode::E);
}

if (settings.inputRemappingEnabled)
{
    // Show rebinding UI; use LoadFromConfig() to apply user changes
}
```

### With SDL2

On Linux, SDL2 is the default backend when `ENABLE_SDL2` is ON (auto-enabled on Linux). The factory can be extended or replaced to return an SDL2 backend. SDL2 requires `libgl-dev` before CMake configure.

### With the VR System

VR controllers map to gamepad key codes. The same action map works for both flat and VR input:

```cpp
actions.RegisterAction("Grab",
    PlatformKeyCode::Gamepad_RT,      // VR trigger or gamepad RT
    PlatformKeyCode::Mouse_Left);     // Fallback for flat mode
```

### With the Editor

The editor uses `InputActionMap` for editor-specific shortcuts separate from game actions:

```cpp
Spark::Input::InputActionMap editorActions;
editorActions.RegisterAction("Undo", PlatformKeyCode::Z); // Ctrl handled separately
editorActions.RegisterAction("Save", PlatformKeyCode::S);
editorActions.RegisterAction("ToggleGrid", PlatformKeyCode::G);
```

## API Reference

### IPlatformInputBackend

| Method | Signature | Description |
|---|---|---|
| `Initialize` | `virtual bool Initialize() = 0` | Initialize the backend |
| `Shutdown` | `virtual void Shutdown() = 0` | Release backend resources |
| `PollEvents` | `virtual void PollEvents() = 0` | Poll OS events and update internal key states |
| `IsKeyDown` | `virtual bool IsKeyDown(PlatformKeyCode key) const = 0` | Check if a key/button is currently held |
| `GetAnalogValue` | `virtual float GetAnalogValue(PlatformKeyCode key) const = 0` | Get analog value (-1 to 1 for axes, 0/1 for buttons) |
| `GetMousePosition` | `virtual void GetMousePosition(float& x, float& y) const = 0` | Get cursor position in pixels |
| `GetMouseDelta` | `virtual void GetMouseDelta(float& dx, float& dy) const = 0` | Get mouse movement since last poll |
| `SetCursorVisible` | `virtual void SetCursorVisible(bool visible) = 0` | Show or hide the OS cursor |
| `GetBackendName` | `virtual std::string_view GetBackendName() const = 0` | Human-readable backend name |

### InputActionMap

| Method | Signature | Description |
|---|---|---|
| `RegisterAction` | `void RegisterAction(const std::string& name, PlatformKeyCode primary, PlatformKeyCode secondary = PlatformKeyCode::None)` | Register a named action with key bindings |
| `UpdateActions` | `void UpdateActions(const IPlatformInputBackend& backend)` | Update all action states from backend (call once per frame) |
| `IsActionPressed` | `bool IsActionPressed(const std::string& name) const` | Check if an action is currently held |
| `IsActionJustPressed` | `bool IsActionJustPressed(const std::string& name) const` | Check if an action was just pressed this frame |
| `GetActionAnalog` | `float GetActionAnalog(const std::string& name) const` | Get analog value with deadzone applied |
| `GetAction` | `const InputAction* GetAction(const std::string& name) const` | Get read-only pointer to an action (nullptr if not found) |
| `RemoveAction` | `void RemoveAction(const std::string& name)` | Remove a registered action |
| `GetAllActions` | `const std::unordered_map<std::string, InputAction>& GetAllActions() const` | Get all registered actions |
| `SaveToConfig` | `std::string SaveToConfig() const` | Serialize bindings to JSON |
| `LoadFromConfig` | `void LoadFromConfig(std::string_view json)` | Deserialize bindings from JSON |

### PlatformInputFactory

| Method | Signature | Description |
|---|---|---|
| `CreateBackend` | `static std::unique_ptr<IPlatformInputBackend> CreateBackend()` | Create platform-appropriate backend (falls back to NullInputBackend) |

## Thread Safety

`InputActionMap` is **not thread-safe**. `UpdateActions()`, `RegisterAction()`, and all query methods access the internal action map without synchronization. Call from a single thread (the main/game thread).

`IPlatformInputBackend` implementations (`PollEvents`, `IsKeyDown`, etc.) should be called from the same thread that created the backend. OS event queues are typically bound to the thread that created the window.

`PlatformInputFactory::CreateBackend()` is a static factory method with no shared mutable state and is safe to call from any thread, though the returned backend should be used from one thread.

## See Also

- [Accessibility](Accessibility.md) -- One-handed mode and input remapping integration
- [Telemetry-System](../advanced/Telemetry-System.md) -- Record input analytics events
- [VR-System](VR-System.md) -- VR controller input mapping
- [Architecture-Overview](../getting-started/Architecture-Overview.md) -- System initialization order
