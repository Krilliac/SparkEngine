# Input System

The `InputManager` handles keyboard, mouse, and gamepad input with frame-based state tracking, key bindings, and console integration.

**Source:** `SparkEngine/Source/Input/InputManager.h`, `SparkEngine/Source/Input/GamepadInput.h`

## Initialization

```cpp
InputManager input;
input.Initialize(hwnd);  // Pass the window handle
```

## Keyboard Input

```cpp
// Check if a key is currently held down
if (input.IsKeyDown(VK_SPACE)) {
    // Space is held
}

// Check if a key was just pressed this frame
if (input.WasKeyPressed('W')) {
    // W was pressed this frame
}

// Check if a key was just released this frame
if (input.WasKeyReleased(VK_SHIFT)) {
    // Shift was released this frame
}
```

## Mouse Input

```cpp
// Mouse button state (0 = Left, 1 = Right, 2 = Middle)
if (input.IsMouseButtonDown(0)) {
    // Left mouse button held
}

// Mouse position
int mouseX = input.GetMouseX();
int mouseY = input.GetMouseY();

// Mouse movement delta (since last frame)
int deltaX = input.GetMouseDeltaX();
int deltaY = input.GetMouseDeltaY();
```

## Mouse Capture

For first-person camera controls, capture the mouse to hide the cursor and track relative movement:

```cpp
input.CaptureMouse();    // Capture and hide cursor
input.ReleaseMouse();    // Release and show cursor
bool captured = input.IsMouseCaptured();
```

## Key Bindings

Map action names to keys for configurable controls:

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
    // Jump action
}
```

## Input Settings

```cpp
input.SetMouseSensitivity(1.0f);   // Mouse sensitivity multiplier
input.SetMouseDeadZone(0.01f);     // Ignore tiny mouse movements
input.SetMouseAcceleration(false); // Disable mouse acceleration
input.SetInvertMouseY(false);      // Invert Y axis
input.SetRawMouseInput(true);      // Use raw mouse input (bypasses OS acceleration)
```

## Gamepad Input

`GamepadInput` provides controller support:

```cpp
GamepadInput gamepad;
float leftStickX  = gamepad.GetLeftStickX();
float leftStickY  = gamepad.GetLeftStickY();
float rightStickX = gamepad.GetRightStickX();
float rightStickY = gamepad.GetRightStickY();
float leftTrigger = gamepad.GetLeftTrigger();
bool  aButton     = gamepad.IsButtonDown(GamepadButton::A);
```

## Default FPS Controls

| Input | Action |
|-------|--------|
| W / A / S / D | Move forward / left / back / right |
| Mouse | Look around |
| Space | Jump |
| Ctrl | Crouch |
| Left Click | Fire / Capture mouse |
| Esc | Release mouse / Menu |
| ` (Backtick) | Toggle debug console |

## Frame Update

Call `Update()` once per frame to process input state changes:

```cpp
// In game loop
input.Update();  // Store current states, calculate deltas
```

## Thread Safety

Input state access is protected by a mutex for thread-safe reads. However, `Update()` and `ProcessMessage()` should only be called from the main thread.

## Console Commands

```
input_info           # Show input system status
input_sensitivity <v> # Set mouse sensitivity
input_deadzone <v>    # Set mouse dead zone
input_invert_y       # Toggle Y-axis inversion
input_raw <on|off>   # Toggle raw mouse input
input_bindings       # List all key bindings
input_log <on|off>   # Toggle input event logging
input_stats          # Show input statistics
```

## See Also

- [[Creating a Game Module]] — Accessing InputManager via IEngineContext
- [[SparkConsole]] — Input debug commands
