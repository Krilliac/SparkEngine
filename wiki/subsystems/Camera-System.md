# Camera System

The Camera System provides a first-person camera with smooth movement, mouse look, zoom, and console integration. It consists of a single class, `SparkEngineCamera`, used by the game module and graphics engine for view and projection matrix generation.

**Source:** `SparkEngine/Source/Camera/`

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                       Game Loop                          │
│  input → Player → Camera.MoveForward/Yaw/Pitch → Update │
└──────────────────────────┬──────────────────────────────┘
                           │
                           ▼
              ┌────────────────────────┐
              │   SparkEngineCamera    │
              │  Position, Rotation    │
              │  View & Projection     │
              │  Zoom, Smooth Transitions │
              │  Console Integration   │
              └────────────┬───────────┘
                           │
                           ▼
              ┌────────────────────────┐
              │    GraphicsEngine      │
              │  GetViewMatrix()       │
              │  GetProjectionMatrix() │
              └────────────────────────┘
```

- **Single class:** `SparkEngineCamera`
- **Coordinate system:** Right-handed, Y-up (DirectXMath)
- **Thread-safe:** All state access protected by `m_stateMutex`
- **Pitch clamping:** Automatically clamped to approximately +/-89.4 degrees to prevent gimbal lock at the poles

### Source Files

| File | Responsibility |
|------|---------------|
| `SparkEngineCamera.h` | Class declaration, `CameraState` struct, all public methods |
| `SparkEngineCamera.cpp` | Implementation: matrix math, movement, rotation, console methods |

---

## Initialization and Update

```cpp
SparkEngineCamera camera;
camera.Initialize(16.0f / 9.0f);  // Must be called before gameplay

// In game loop:
camera.Update(deltaTime);  // Recalculates view matrix, processes smooth transitions
```

`Initialize()` sets the aspect ratio and builds the initial projection and view matrices. `Update()` must be called once per frame -- it updates the view matrix and advances any active smooth transition.

---

## Movement

All movement methods multiply the input amount by `m_moveSpeed` and translate along the corresponding basis vector.

```cpp
// Relative movement (scaled by move speed)
camera.MoveForward(amount);   // Along forward vector (positive = forward)
camera.MoveRight(amount);     // Along right vector (positive = right)
camera.MoveUp(amount);        // Along up vector (positive = up)

// Absolute positioning
camera.SetPosition(XMFLOAT3(10.0f, 5.0f, -3.0f));
```

---

## Rotation

Rotation methods multiply the input angle by `m_rotationSpeed` and `m_mouseSensitivity`. Pitch and Yaw also respect the `m_invertY` flag (Pitch only).

```cpp
camera.Pitch(angle);   // X-axis rotation, clamped to ~+/-89.4 degrees
camera.Yaw(angle);     // Y-axis rotation, wraps at 360 degrees
camera.Roll(angle);    // Z-axis rotation, wraps at 360 degrees
```

Pitch is clamped to `[-PI/2 + 0.01, PI/2 - 0.01]` radians to prevent over-rotation.

---

## Zoom

Toggles between default and zoomed field of view. Rebuilds the projection matrix immediately.

```cpp
camera.SetZoom(true);    // Switch to zoomed FOV (default: 45 degrees)
camera.SetZoom(false);   // Switch to normal FOV (default: 90 degrees)
```

---

## Matrix Access

```cpp
const XMMATRIX& view = camera.GetViewMatrix();
const XMMATRIX& proj = camera.GetProjectionMatrix();

const XMFLOAT3& pos     = camera.GetPosition();
const XMFLOAT3& forward = camera.GetForward();
XMFLOAT3 rotation       = camera.GetRotation();   // (pitch, yaw, roll) in radians
```

`GetPosition()`, `GetForward()`, and `GetRotation()` are thread-safe (acquire `m_stateMutex`).

---

## Smooth Transitions

The camera supports smooth interpolation from one position to another using SmoothStep easing:

```cpp
camera.Console_SmoothMoveTo(targetX, targetY, targetZ, durationSeconds);

// Check if still transitioning
if (camera.IsTransitioning()) { /* movement in progress */ }
```

During a transition, `Update()` interpolates the position each frame using the formula `t^2 * (3 - 2t)` for natural acceleration and deceleration.

---

## CameraState Struct

`Console_GetState()` returns a snapshot of all camera parameters:

```cpp
struct CameraState {
    XMFLOAT3 position;
    XMFLOAT3 rotation;         // In degrees (pitch, yaw, roll)
    XMFLOAT3 forward, right, up;
    float moveSpeed, rotationSpeed, mouseSensitivity;
    float defaultFov, zoomedFov, currentFov;  // In degrees
    float aspectRatio, nearPlane, farPlane;
    bool invertY, smoothMovement, isZoomed;
};
```

---

## Console Integration

All `Console_*` methods are thread-safe and log their actions to `SimpleConsole`.

| Method | Description |
|--------|-------------|
| `Console_SetFOV(float degrees)` | Set default FOV (10-170 degrees) |
| `Console_SetMouseSensitivity(float)` | Set sensitivity multiplier (0.1-10.0) |
| `Console_SetInvertY(bool)` | Toggle Y-axis inversion for Pitch |
| `Console_SetMoveSpeed(float)` | Set movement speed (0.1-100.0) |
| `Console_SetRotationSpeed(float)` | Set rotation speed multiplier (0.1-10.0) |
| `Console_SetPosition(float x, y, z)` | Teleport camera to world position |
| `Console_SetRotation(float pitch, yaw, roll)` | Set rotation in degrees (pitch clamped) |
| `Console_SetClippingPlanes(float near, far)` | Set near (0.01-10.0) and far (100-10000) planes |
| `Console_ResetToDefaults()` | Reset all parameters to defaults |
| `Console_GetState()` | Return `CameraState` snapshot |
| `Console_LookAt(float x, y, z)` | Orient camera to face a world point |
| `Console_SmoothMoveTo(float x, y, z, duration)` | Smooth transition to target position |
| `Console_RegisterStateCallback(fn)` | Register a callback invoked on any state change |

---

## Default Parameters

| Parameter | Default | Range |
|-----------|---------|-------|
| Position | (0, 0, 0) | -- |
| Move Speed | 10.0 | 0.1 - 100.0 |
| Rotation Speed | 2.0 | 0.1 - 10.0 |
| Mouse Sensitivity | 1.0 | 0.1 - 10.0 |
| Default FOV | 90 degrees | 10 - 170 degrees |
| Zoomed FOV | 45 degrees | 10 - 170 degrees |
| Near Plane | 0.1 | 0.01 - 10.0 |
| Far Plane | 1000.0 | 100 - 10000 |
| Aspect Ratio | 16:9 (1.777) | Set via `Initialize()` |
| Invert Y | false | -- |
| Smooth Movement | true | -- |

---

## Integration with Game Loop

A typical integration flow in the game module:

```cpp
// Startup
SparkEngineCamera camera;
camera.Initialize(windowWidth / windowHeight);

// Per frame
inputManager.Update();

float dt = timer.GetDeltaTime();
float moveAmount = dt;

if (input.IsKeyDown('W')) camera.MoveForward(moveAmount);
if (input.IsKeyDown('S')) camera.MoveForward(-moveAmount);
if (input.IsKeyDown('A')) camera.MoveRight(-moveAmount);
if (input.IsKeyDown('D')) camera.MoveRight(moveAmount);

int dx, dy;
if (input.GetMouseDelta(dx, dy)) {
    camera.Yaw(dx * dt);
    camera.Pitch(dy * dt);
}

camera.Update(dt);

// Pass matrices to renderer
graphicsEngine.SetViewMatrix(camera.GetViewMatrix());
graphicsEngine.SetProjectionMatrix(camera.GetProjectionMatrix());
```

---

## Thread Safety

| Operation | Thread Safety | Details |
|-----------|--------------|---------|
| `GetPosition()`, `GetForward()`, `GetRotation()` | Thread-safe | Acquires `m_stateMutex` |
| `SetPosition()` | Thread-safe | Acquires `m_stateMutex`, updates view matrix |
| All `Console_*` methods | Thread-safe | Each acquires `m_stateMutex` |
| `Console_GetState()` | Thread-safe | Returns a full copy under lock |
| `MoveForward/Right/Up`, `Pitch`, `Yaw`, `Roll` | Thread-safe | Each acquires `m_stateMutex` |
| `Update()` | Main thread | Should be called once per frame from the main thread |

All mutable state access is protected by `m_stateMutex`. The state callback (`m_stateCallback`) is invoked while the mutex is held, so callbacks must not re-enter camera methods.

---

## Error Handling

- `Initialize()` asserts that the aspect ratio is positive.
- `Update()` asserts that `deltaTime` is non-negative and finite.
- Movement and rotation methods assert that input values are finite.
- Console methods with ranges log an error to `SimpleConsole` and return without modifying state if the value is out of range.
- `Console_SetClippingPlanes()` additionally validates that `nearPlane < farPlane`.

---

## See Also

- [Input System](Input-System.md) -- Keyboard and mouse input that drives camera movement
- [Rendering and Graphics](Rendering-and-Graphics.md) -- Consumes view and projection matrices
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Player controller that owns the camera
- [SparkConsole](../gameplay-tools/SparkConsole.md) -- Console commands for camera tuning
- [Cinematic Sequencer](../gameplay-tools/Cinematic-Sequencer.md) -- CameraPathTrack for scripted camera motion
- [Creating a Game Module](../getting-started/Creating-a-Game-Module.md) -- Accessing the camera via IEngineContext
