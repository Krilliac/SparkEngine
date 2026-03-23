# 13 — Input System

**Location:** `SparkEngine/Source/Input/`

Comprehensive input handling for keyboard, mouse, and gamepad with key bindings, remapping, sensitivity control, and console integration.

---

## InputManager

**File:** `SparkEngine/Source/Input/InputManager.h`

### Initialization

```cpp
InputManager input;
input.Initialize(hwnd);  // Windows: hooks into window message loop
```

### Keyboard

```cpp
bool isDown = input.IsKeyDown(VK_SPACE);         // Currently held
bool justPressed = input.IsKeyPressed(VK_SPACE);  // Edge: up→down this frame
bool justReleased = input.IsKeyReleased(VK_SPACE);// Edge: down→up this frame
```

State tracking uses current + previous frame arrays for edge detection.

### Mouse

```cpp
bool leftDown = input.IsMouseButtonDown(MouseButton::Left);
bool rightPressed = input.IsMouseButtonPressed(MouseButton::Right);

POINT pos = input.GetMousePosition();
POINT delta = input.GetMouseDelta();

input.SetMouseCapture(true);       // Lock cursor to window
input.SetMouseSensitivity(2.0f);
input.SetInvertY(false);
input.SetRawInput(true);           // Use raw input API (bypasses acceleration)
```

### Windows Message Processing

```cpp
// In window proc
case WM_KEYDOWN:    input.OnKeyDown(wParam); break;
case WM_KEYUP:      input.OnKeyUp(wParam); break;
case WM_LBUTTONDOWN: input.OnMouseButtonDown(MouseButton::Left); break;
case WM_LBUTTONUP:   input.OnMouseButtonUp(MouseButton::Left); break;
case WM_MOUSEMOVE:    input.OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); break;
```

---

## Input Bindings

**File:** `SparkEngine/Source/Input/InputBindings.h`

Action-to-key mapping with remapping:

```cpp
InputBindings bindings;

// Bind actions
bindings.Bind("MoveForward", VK_W);
bindings.Bind("MoveBackward", VK_S);
bindings.Bind("MoveLeft", VK_A);
bindings.Bind("MoveRight", VK_D);
bindings.Bind("Jump", VK_SPACE);
bindings.Bind("Crouch", VK_LCONTROL);
bindings.Bind("Sprint", VK_LSHIFT);
bindings.Bind("Fire", MouseButton::Left);
bindings.Bind("Aim", MouseButton::Right);
bindings.Bind("Reload", VK_R);
bindings.Bind("Interact", VK_E);

// Query
bool moving = bindings.IsActionActive("MoveForward");
bool fired = bindings.IsActionJustPressed("Fire");

// Remap
bindings.Rebind("Jump", VK_UP);
bindings.Save("Data/Config/bindings.ini");
bindings.Load("Data/Config/bindings.ini");
```

---

## Gamepad Input

**File:** `SparkEngine/Source/Input/GamepadInput.h`

XInput-based controller support:

```cpp
GamepadInput gamepad;
gamepad.Update();

// Axes (analog, -1.0 to 1.0 for sticks, 0.0 to 1.0 for triggers)
float moveX = gamepad.GetAxis(GamepadAxis::LeftStickX);
float moveY = gamepad.GetAxis(GamepadAxis::LeftStickY);
float aimX = gamepad.GetAxis(GamepadAxis::RightStickX);
float trigger = gamepad.GetAxis(GamepadAxis::RightTrigger);

// Buttons (digital)
bool jump = gamepad.IsButtonDown(GamepadButton::A);
bool fire = gamepad.IsButtonDown(GamepadButton::RightBumper);

// Deadzone
gamepad.SetDeadzone(0.15f);
```

### GamepadButton Enum

`A`, `B`, `X`, `Y`, `LeftBumper`, `RightBumper`, `Back`, `Start`, `LeftStickClick`, `RightStickClick`, `DPadUp`, `DPadDown`, `DPadLeft`, `DPadRight`

### GamepadAxis Enum

`LeftStickX`, `LeftStickY`, `RightStickX`, `RightStickY`, `LeftTrigger`, `RightTrigger`

---

## Console Integration

```cpp
InputMetrics metrics = input.Console_GetMetrics();
// keyPressCount, mousePressCount, totalMouseDistance

input.Console_SetSensitivity(2.5f);
input.Console_SetDeadzone(0.15f);
input.Console_SetAcceleration(1.0f);
```

---

## Thread Safety

InputManager is thread-safe with mutex protection on state and atomic counters for metrics.
