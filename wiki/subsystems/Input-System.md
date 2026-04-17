# Input System

The Input System handles keyboard, mouse, and gamepad input with frame-based state tracking, key bindings, and accessibility features. It consists of three main classes: `InputManager` for direct keyboard/mouse input, `GamepadInput` for controller support, and `InputBindingManager` for rebindable controls and presets.

**Source:** `SparkEngine/Source/Input/`

---

## Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│                         Game Code                                 │
│  input.IsKeyDown('W')  |  input.IsActionDown("Jump")             │
│  gamepad.GetLeftStick() |  bindings.IsActionActive("Sprint")      │
└───────────────┬─────────────────┬───────────────────────────────┘
                │                 │
                ▼                 ▼
┌───────────────────┐  ┌──────────────────┐
│   InputManager    │  │   GamepadInput   │
│  (keyboard/mouse) │  │  (XInput gamepads│
│  Win32/SDL2 msgs  │  │   up to 4)       │
└───────────────────┘  └──────────────────┘

┌───────────────────────────────────────────────────────────────────┐
│                    InputBindingManager                             │
│  Rebindable controls, presets ("WASD", "ESDF", "Controller"),     │
│  accessibility flags, JSON persistence                            │
└───────────────────────────────────────────────────────────────────┘
```

### Source Files

| File | Responsibility |
|------|---------------|
| `InputManager.h` | Keyboard and mouse input, frame-based state tracking, console integration |
| `GamepadInput.h` | Xbox controller support via XInput, dead zones, vibration, action mapping |
| `InputBindings.h` | Runtime rebinding, presets, accessibility flags, JSON serialization |

---

## InputManager

The `InputManager` class handles all keyboard and mouse input processing via Windows messages.

### Initialization

```cpp
InputManager input;
input.Initialize(hwnd);  // Pass the window handle
```

### Frame Update

Call `Update()` once per frame before querying input states:

```cpp
// In game loop:
input.Update();  // Copies current states to previous, calculates deltas
```

### Keyboard Input

```cpp
// Currently held down (returns true every frame while held)
if (input.IsKeyDown(VK_SPACE)) { /* Space is held */ }

// Currently not pressed
if (input.IsKeyUp('W')) { /* W is not pressed */ }

// Just pressed this frame (rising edge -- true for one frame only)
if (input.WasKeyPressed('W')) { /* W was pressed this frame */ }

// Just released this frame (falling edge -- true for one frame only)
if (input.WasKeyReleased(VK_SHIFT)) { /* Shift was released this frame */ }
```

### Mouse Input

```cpp
// Mouse button state (0 = Left, 1 = Right, 2 = Middle)
if (input.IsMouseButtonDown(0)) { /* Left mouse button held */ }
if (input.WasMouseButtonPressed(1)) { /* Right mouse just clicked */ }
if (input.WasMouseButtonReleased(2)) { /* Middle mouse just released */ }

// Mouse position (window-relative pixels)
int x, y;
input.GetMousePosition(x, y);

// Mouse movement delta (since last frame, with sensitivity/deadzone applied)
int deltaX, deltaY;
if (input.GetMouseDelta(deltaX, deltaY)) {
    // Valid delta values available
    camera.Rotate(deltaX * sensitivity, deltaY * sensitivity);
}
```

### Mouse Capture

For first-person camera controls, capture the mouse to hide the cursor and track relative movement:

```cpp
input.CaptureMouse(true);    // Capture: hide cursor, confine to window
input.CaptureMouse(false);   // Release: show cursor, free movement
bool captured = input.IsMouseCaptured();
```

### Key Bindings

Map action names to virtual key codes for configurable controls:

```cpp
// Bind actions to keys
input.BindKey("MoveForward", 'W');
input.BindKey("MoveBack", 'S');
input.BindKey("MoveLeft", 'A');
input.BindKey("MoveRight", 'D');
input.BindKey("Jump", VK_SPACE);
input.BindKey("Crouch", VK_CONTROL);
input.BindKey("Fire", VK_LBUTTON);

// Check bound actions
if (input.IsActionDown("Jump")) {
    player.Jump();
}
```

### Input Settings

```cpp
input.Console_SetMouseSensitivity(1.0f);    // Sensitivity multiplier (0.1-10.0)
input.Console_SetMouseDeadZone(0.01f);       // Ignore tiny movements (0.0-10.0)
input.Console_SetMouseAcceleration(false);   // Disable OS mouse acceleration
input.Console_SetInvertMouseY(false);        // Normal Y-axis
input.Console_SetRawMouseInput(true);        // Bypass OS acceleration curve
input.Console_SetInputLogging(true);         // Log all input events to console
```

### InputManager Internal State

```cpp
class InputManager {
    // Key state maps
    std::unordered_map<int, bool> m_keyStates;       // Current frame
    std::unordered_map<int, bool> m_prevKeyStates;   // Previous frame

    // Mouse buttons [Left, Right, Middle]
    bool m_mouseButtons[3];
    bool m_prevMouseButtons[3];

    // Mouse position and delta
    int m_mouseX, m_mouseY;
    int m_prevMouseX, m_prevMouseY;
    int m_mouseDeltaX, m_mouseDeltaY;

    // Settings
    float m_mouseSensitivity;   // Multiplier applied to delta
    float m_mouseDeadZone;      // Minimum delta to register
    bool m_mouseAcceleration;   // Enable/disable acceleration
    bool m_invertMouseY;        // Invert Y-axis
    bool m_rawMouseInput;       // Use raw input API
    bool m_inputLogging;        // Log events to console

    // Metrics
    size_t m_keyPressCount;     // Total key presses this session
    size_t m_mousePressCount;   // Total mouse clicks this session
    float m_totalMouseDistance;  // Cumulative mouse movement

    // Thread safety
    mutable std::mutex m_inputMutex;
};
```

### InputMetrics Structure

```cpp
struct InputMetrics {
    size_t keyPressCount;         // Total key presses this session
    size_t mousePressCount;       // Total mouse clicks this session
    float totalMouseDistance;     // Total mouse movement distance
    size_t activeKeys;            // Currently pressed key count
    size_t activeMouseButtons;    // Currently pressed button count
    bool mouseCaptured;           // Mouse capture state
    float mouseSensitivity;       // Current sensitivity
    float mouseDeadZone;          // Current dead zone
    bool mouseAcceleration;       // Acceleration state
    bool invertMouseY;            // Y-axis inversion state
    bool rawMouseInput;           // Raw input state
    bool inputLogging;            // Logging state
    size_t totalKeyBindings;      // Number of active bindings
};
```

### Console Integration Methods

| Method | Description |
|--------|-------------|
| `Console_SetMouseSensitivity(float)` | Set sensitivity (0.1-10.0) |
| `Console_SetMouseDeadZone(float)` | Set dead zone (0.0-10.0) |
| `Console_SetMouseAcceleration(bool)` | Enable/disable acceleration |
| `Console_SetInvertMouseY(bool)` | Toggle Y-axis inversion |
| `Console_SetRawMouseInput(bool)` | Toggle raw input mode |
| `Console_SetInputLogging(bool)` | Toggle input event logging |
| `Console_BindKey(action, keyName)` | Bind key by name string |
| `Console_UnbindKey(action)` | Remove a key binding |
| `Console_ListKeyBindings()` | Get formatted binding list |
| `Console_SimulateKeyPress(keyName, duration)` | Simulate a key press |
| `Console_ClearInputStates()` | Reset all input states |
| `Console_GetRecentEvents(count)` | Get recent input events |
| `Console_IsActionActive(action)` | Check if action is active |
| `Console_GetMetrics()` | Get comprehensive metrics |
| `Console_GetSettings()` | Get current settings |
| `Console_ApplySettings(settings)` | Apply settings structure |
| `Console_ResetToDefaults()` | Reset all settings |
| `Console_RefreshInput()` | Force input system refresh |

---

## GamepadInput

`GamepadInput` provides Xbox controller support for up to 4 simultaneous controllers via XInput.

### Enums

```cpp
enum class GamepadButton {
    A, B, X, Y,                // Face buttons
    LeftBumper, RightBumper,   // Shoulder bumpers (LB/RB)
    Back, Start,               // Menu buttons
    LeftStick, RightStick,     // Thumbstick clicks (L3/R3)
    DPadUp, DPadDown,          // D-pad
    DPadLeft, DPadRight,
    Count                      // Sentinel for array sizing
};

enum class GamepadTrigger { Left, Right };
enum class GamepadStick { Left, Right };

enum class DeadZoneMode {
    Circular,  // Radial dead zone (recommended -- smooth diagonal movement)
    Axial      // Per-axis dead zone (each axis independent)
};
```

### GamepadState

```cpp
struct GamepadState {
    bool connected;                     // Controller connected?
    XINPUT_STATE xinputState;           // Raw XInput state
    XINPUT_STATE prevXinputState;       // Previous frame (for edge detection)
    XMFLOAT2 leftStick, rightStick;    // Processed [-1, 1] with dead zone
    float leftTrigger, rightTrigger;   // Processed [0, 1] with threshold
    float leftMotor, rightMotor;       // Vibration intensity [0, 1]
    float vibrationTimer;              // Remaining vibration time (seconds)
};
```

### Basic Usage

```cpp
GamepadInput gamepad;

// In game loop:
gamepad.Update(deltaTime);

// Connection
if (gamepad.IsConnected()) {
    // Thumbsticks (dead-zone-corrected, [-1, 1])
    XMFLOAT2 leftStick = gamepad.GetLeftStick();
    float moveX = leftStick.x;
    float moveY = leftStick.y;

    XMFLOAT2 rightStick = gamepad.GetRightStick();
    camera.Rotate(rightStick.x, rightStick.y);

    // Individual axis access
    float lx = gamepad.GetLeftStickX();
    float ly = gamepad.GetLeftStickY();
    float rx = gamepad.GetRightStickX();
    float ry = gamepad.GetRightStickY();

    // Triggers ([0, 1])
    float leftTrigger = gamepad.GetLeftTrigger();
    float rightTrigger = gamepad.GetRightTrigger();
    if (gamepad.IsTriggerDown(GamepadTrigger::Right, 0.3f)) { /* Shooting */ }

    // Buttons
    if (gamepad.IsButtonDown(GamepadButton::A)) { /* A held */ }
    if (gamepad.WasButtonPressed(GamepadButton::X)) { /* X just pressed */ }
    if (gamepad.WasButtonReleased(GamepadButton::B)) { /* B just released */ }
}
```

### Vibration / Rumble

```cpp
// Set vibration (left = low frequency rumble, right = high frequency buzz)
gamepad.SetVibration(0.5f, 0.3f, 0.5f);  // 50% left, 30% right, 0.5s duration
gamepad.SetVibration(1.0f, 1.0f);          // Full intensity, indefinite

// Stop
gamepad.StopVibration();       // Stop controller 0
gamepad.StopAllVibrations();   // Stop all controllers
```

### Dead Zone Configuration

```cpp
gamepad.SetDeadZone(GamepadStick::Left, 0.24f);   // Default XInput left stick
gamepad.SetDeadZone(GamepadStick::Right, 0.26f);  // Default XInput right stick
gamepad.SetDeadZoneMode(DeadZoneMode::Circular);   // Radial (recommended)
gamepad.SetTriggerThreshold(0.12f);                 // Minimum trigger activation
gamepad.SetStickSensitivity(1.5f);                  // 1.5x sensitivity multiplier
```

### Action Mapping

```cpp
// Bind named actions to gamepad inputs
gamepad.BindAction("Jump", GamepadButton::A);
gamepad.BindAction("Crouch", GamepadButton::B);
gamepad.BindAction("Shoot", GamepadTrigger::Right, 0.3f);  // Trigger with threshold
gamepad.BindAction("AimDownSights", GamepadTrigger::Left, 0.2f);

// Query by action name
if (gamepad.IsActionDown("Jump")) { player.Jump(); }
if (gamepad.WasActionPressed("Shoot")) { weapon.Fire(); }
```

### Multi-Controller Support

All methods accept an optional `controllerIndex` parameter (0-3):

```cpp
// Player 2's controller
if (gamepad.IsConnected(1)) {
    auto stick = gamepad.GetLeftStick(1);
    if (gamepad.WasButtonPressed(GamepadButton::Start, 1)) { /* P2 joined */ }
    gamepad.SetVibration(0.5f, 0.5f, 0.3f, 1);  // Rumble controller 1
}

int connectedCount = gamepad.GetConnectedCount();  // 0-4
```

### Connection Polling

Disconnected controllers are polled at a reduced rate (every 2 seconds) to avoid XInput performance overhead:

```cpp
static constexpr float CONNECTION_POLL_INTERVAL = 2.0f;
```

---

## InputBindingManager

The `InputBindingManager` provides rebindable controls, presets, and accessibility features.

### InputBinding Structure

```cpp
struct InputBinding {
    int primaryKey      = 0;        // Primary keyboard key code
    int alternateKey    = 0;        // Alternate key (0 = none)
    int gamepadButton   = -1;       // Gamepad button index (-1 = none)
    float gamepadAxis   = 0.0f;     // Gamepad axis for analog actions
    bool holdToToggle   = false;    // Tap toggles on/off instead of hold
};
```

### Basic Usage

```cpp
InputBindingManager bindings;

// Set individual bindings
bindings.SetBinding("MoveForward", InputBinding{'W'});
bindings.SetBinding("Jump", InputBinding{VK_SPACE});

// Check for conflicts
std::string conflict = bindings.FindConflict("Jump");
if (!conflict.empty()) { /* "Jump" conflicts with another action */ }

// Get all bindings
const auto& allBindings = bindings.GetAllBindings();
```

### Presets

```cpp
// Create built-in presets
bindings.CreateDefaultPresets();   // Creates "Default", "Left-Handed", "Accessibility"

// Apply a preset (replaces all current bindings)
bindings.ApplyPreset("WASD");

// List available presets
for (const auto& name : bindings.GetPresetNames()) {
    LOG("Preset: " + name);
}

// Register a custom preset
InputPreset customPreset;
customPreset.name = "Custom";
customPreset.description = "My custom layout";
customPreset.bindings["Jump"] = InputBinding{VK_SPACE};
// ...
bindings.RegisterPreset(customPreset);
```

### Accessibility Flags

```cpp
enum class AccessibilityFlags : uint32_t {
    None                   = 0,
    HoldToToggle           = 1 << 0,   // Convert hold actions to toggle
    LargeSubtitles         = 1 << 1,   // Increase subtitle font size
    HighContrastUI         = 1 << 2,   // High contrast UI mode
    ColorblindProtanopia   = 1 << 3,   // Red-green (protan) filter
    ColorblindDeuteranopia = 1 << 4,   // Red-green (deutan) filter
    ColorblindTritanopia   = 1 << 5,   // Blue-yellow filter
    ReducedMotion          = 1 << 6,   // Reduce screen shake/motion
    ScreenNarrator         = 1 << 7,   // Screen reader support
    AutoAim                = 1 << 8,   // Aim assist
    OneTouchMode           = 1 << 9    // Simplified single-button input
};

// Enable accessibility features
bindings.SetAccessibility(
    AccessibilityFlags::HoldToToggle |
    AccessibilityFlags::AutoAim |
    AccessibilityFlags::LargeSubtitles
);

// Check individual flags
if (bindings.IsAccessibilityEnabled(AccessibilityFlags::ColorblindProtanopia)) {
    ApplyColorblindFilter();
}
```

### Persistence

```cpp
// Save bindings + accessibility settings to JSON
bindings.SaveToFile("Config/keybinds.json");

// Load from JSON
bindings.LoadFromFile("Config/keybinds.json");
```

### Change Notifications

```cpp
bindings.OnBindingChanged([](const std::string& action, const InputBinding& binding) {
    UpdateControlsUI(action, binding);
});
```

---

## Default FPS Controls

| Input | Action |
|-------|--------|
| W / A / S / D | Move forward / left / back / right |
| Mouse movement | Look around (yaw/pitch) |
| Space | Jump |
| Ctrl | Crouch |
| Left Click | Fire / Capture mouse |
| Right Click | Aim down sights |
| Esc | Release mouse / Pause menu |
| ` (Backtick) | Toggle debug console |
| Tab | Scoreboard |
| R | Reload |
| E | Interact |

---

## Thread Safety

| Component | Thread Safety | Details |
|-----------|--------------|---------|
| `InputManager` | Partial | Read access protected by `m_inputMutex`. `Update()` and `HandleMessage()` must be called from main thread only. |
| `GamepadInput` | Single-threaded | Call all methods from main thread. XInput is not thread-safe. |
| `InputBindingManager` | Single-threaded | Designed for main-thread access only. |

---

## Error Handling

- `InputManager::Initialize()` requires a valid `HWND`. Passing `nullptr` will result in mouse capture failures.
- `GamepadInput` returns safe defaults (0, false, {0,0}) for disconnected or out-of-range controller indices.
- `InputBindingManager::LoadFromFile()` returns `false` if the JSON file is missing or malformed; existing bindings are preserved.
- Key name lookups return `KeyCode::Unknown` (or equivalent) for unrecognized names.

---

## Performance Considerations

- **State tracking**: Key states use `std::unordered_map` for sparse key coverage (only pressed keys are stored).
- **Gamepad polling**: Connected controllers are polled every frame; disconnected controllers are polled every 2 seconds to minimize XInput overhead.
- **Dead zone processing**: Circular dead zone uses a single `sqrt()` per stick per frame. Axial mode uses no `sqrt()`.
- **Event callbacks**: Dispatched synchronously during `Update()`. Keep callbacks lightweight to avoid frame time impact.
- **Platform backend**: Win32 backend processes queued messages; SDL2 backend calls `SDL_PollEvent()`.

---

## Console Commands

```
input_info                # Input system status (mouse state, key count, gamepad connection)
input_sensitivity <v>     # Set mouse sensitivity multiplier (0.1-10.0)
input_deadzone <v>        # Set mouse dead zone threshold (0.0-10.0)
input_invert_y            # Toggle Y-axis inversion for mouse look
input_raw <on|off>        # Toggle raw mouse input (bypasses OS acceleration)
input_bindings            # List all current key bindings
input_log <on|off>        # Toggle input event logging to console
input_stats               # Show input statistics (press counts, mouse distance)
```

---

## Troubleshooting

### Mouse look is jittery or too sensitive
- Reduce sensitivity: `input_sensitivity 0.5`
- Enable raw mouse input: `input_raw on`
- Increase dead zone: `input_deadzone 0.05`
- Disable mouse acceleration: `Console_SetMouseAcceleration(false)`

### Gamepad not detected
- Verify controller is connected via Windows Settings
- Check `gamepad.IsConnected()` -- disconnected controllers are polled every 2 seconds
- XInput only supports Xbox-compatible controllers; other gamepads may need SDL2 backend

### Keys not responding
- Check `input_info` for system status
- Verify `Initialize()` was called with a valid window handle
- Ensure `Update()` is called every frame before input queries
- Check that `HandleMessage()` is being called from `WndProc`

### Bindings not saving/loading
- Verify file path exists and is writable
- Check JSON format in the bindings file
- `LoadFromFile()` returns `false` on failure; check return value

### Input lag
- Ensure `Update()` is called at the start of the frame, before any input queries
- Reduce input processing time by minimizing event callback work
- Enable raw mouse input to bypass OS processing pipeline

---

## See Also

- [Creating a Game Module](../getting-started/Creating-a-Game-Module.md) -- Accessing InputManager via IEngineContext
- [SparkConsole](../gameplay-tools/SparkConsole.md) -- Input debug commands
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Player controller and FPS controls
- [Scripting with AngelScript](Scripting-with-AngelScript.md) -- Input API available in scripts
- [Event System](Event-System.md) -- Input-related event publishing
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Input binding configuration UI
