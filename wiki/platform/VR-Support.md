# VR Support

SparkEngine provides a VR/AR integration framework designed for OpenXR. It handles head tracking, stereoscopic rendering, motion controller input, and haptic feedback. Currently a framework stub requiring the OpenXR SDK and a VR runtime (SteamVR, Oculus, WMR).

**Source:** `SparkEngine/Source/Engine/VR/VRSystem.h`

**Namespace:** `Spark::VR`

**CMake toggle:** `ENABLE_VR=ON`

---

## Architecture Overview

```
+--------------------------------------------------------------+
|                         VRSystem                              |
|  (per-instance, not singleton)                                |
|                                                               |
|  m_initialized: bool                                          |
|  m_headPosition: XMFLOAT3        m_headOrientation: XMFLOAT4 |
|  m_leftEye: VREye                m_rightEye: VREye            |
|  m_leftController: VRController  m_rightController: VRController|
|  m_trackingSpace: VRTrackingSpace (default: RoomScale)        |
|  m_recommendedWidth: int (1440)  m_recommendedHeight: int (1600)|
|                                                               |
|  Initialize() / Shutdown()                                    |
|  UpdateTracking()                                             |
|  TriggerHaptic()                                              |
|  RecenterTracking()                                           |
+------------------+-------------------+-----------------------+
                   |                   |
        +----------+------+   +--------+--------+
        |     VREye        |   |  VRController    |
        | (per-eye data)   |   | (controller state)|
        |                  |   |                   |
        | viewMatrix       |   | connected         |
        | projectionMatrix |   | position           |
        | position         |   | orientation        |
        | viewportWidth    |   | velocity           |
        | viewportHeight   |   | angularVelocity    |
        +------------------+   | triggerValue       |
                               | gripValue          |
                               | thumbstick         |
                               | buttonMask         |
                               +-------------------+
```

## Key Structs and Enums

| Type | Responsibility |
|------|---------------|
| `VRSystem` | Hardware initialization, tracking updates, per-eye rendering data, controller state, haptic feedback, and console integration |
| `VRController` | Snapshot of a single motion controller's state: position, orientation, velocity, trigger/grip axes, thumbstick, and button mask |
| `VREye` | Per-eye rendering data: view matrix, projection matrix, eye world position, and recommended viewport dimensions |
| `VRTrackingSpace` | Enum selecting between `Seated` and `RoomScale` tracking modes |

---

## Quick Start

```cpp
#include "Engine/VR/VRSystem.h"
using namespace Spark::VR;

VRSystem vr;
if (vr.Initialize())
{
    vr.SetTrackingSpace(VRTrackingSpace::RoomScale);

    // Main loop
    while (running)
    {
        // Update tracking data once per frame
        vr.UpdateTracking();

        // Read head pose
        auto headPos = vr.GetHeadPosition();    // XMFLOAT3
        auto headOri = vr.GetHeadOrientation(); // XMFLOAT4 (quaternion)

        // Render left eye
        const auto& leftEye = vr.GetLeftEye();
        RenderScene(leftEye.viewMatrix, leftEye.projectionMatrix);

        // Render right eye
        const auto& rightEye = vr.GetRightEye();
        RenderScene(rightEye.viewMatrix, rightEye.projectionMatrix);

        // Handle controller input
        ProcessControllerInput(vr);
    }

    vr.Shutdown();
}
```

---

## VRController Struct

The `VRController` struct holds the complete state of a single motion controller, updated each frame by `VRSystem::UpdateTracking()`.

```cpp
struct VRController
{
    bool connected = false;                        // Is the controller active?
    DirectX::XMFLOAT3 position{0, 0, 0};          // World-space position
    DirectX::XMFLOAT4 orientation{0, 0, 0, 1};    // Orientation quaternion
    DirectX::XMFLOAT3 velocity{0, 0, 0};          // Linear velocity (m/s)
    DirectX::XMFLOAT3 angularVelocity{0, 0, 0};   // Angular velocity (rad/s)

    float triggerValue = 0.0f;                     // Trigger axis (0.0 to 1.0)
    float gripValue = 0.0f;                        // Grip axis (0.0 to 1.0)
    DirectX::XMFLOAT2 thumbstick{0, 0};           // Thumbstick x,y (-1.0 to 1.0)
    uint32_t buttonMask = 0;                       // Bitmask of pressed buttons
};
```

### Member Details

| Member | Type | Range | Description |
|--------|------|-------|-------------|
| `connected` | `bool` | true/false | Whether this controller is currently tracked and active. Always check before reading other fields. |
| `position` | `XMFLOAT3` | world-space | Position in the tracking space, in meters. Origin depends on `VRTrackingSpace`. |
| `orientation` | `XMFLOAT4` | unit quaternion | Rotation as a quaternion (x, y, z, w). `w=1` is identity. |
| `velocity` | `XMFLOAT3` | m/s | Linear velocity. Useful for throw estimation and gesture detection. |
| `angularVelocity` | `XMFLOAT3` | rad/s | Angular velocity around each axis. Useful for spin-throw gestures. |
| `triggerValue` | `float` | 0.0 - 1.0 | Analog trigger pull depth. 0.0 = released, 1.0 = fully pressed. |
| `gripValue` | `float` | 0.0 - 1.0 | Analog grip squeeze depth. Some controllers have binary grips (0 or 1). |
| `thumbstick` | `XMFLOAT2` | -1.0 to 1.0 | Thumbstick deflection on x (left/right) and y (forward/back) axes. |
| `buttonMask` | `uint32_t` | bitmask | Bitfield of currently pressed buttons. Use bitwise AND to check specific buttons. |

### Reading Controller Input

```cpp
const auto& left = vr.GetLeftController();
const auto& right = vr.GetRightController();

if (right.connected)
{
    // Position and orientation
    auto pos = right.position;
    auto ori = right.orientation;

    // Analog inputs
    float trigger = right.triggerValue;      // 0.0 to 1.0
    float grip = right.gripValue;            // 0.0 to 1.0
    auto stick = right.thumbstick;           // x,y: -1.0 to 1.0

    // Button state
    uint32_t buttons = right.buttonMask;

    // Velocity (useful for throw physics)
    auto vel = right.velocity;
    auto angVel = right.angularVelocity;
}
```

---

## VREye Struct

The `VREye` struct contains all data needed to render a single eye's view.

```cpp
struct VREye
{
    DirectX::XMFLOAT4X4 viewMatrix;           // Eye view matrix
    DirectX::XMFLOAT4X4 projectionMatrix;     // Eye projection matrix
    DirectX::XMFLOAT3 position{0, 0, 0};      // Eye world position
    int viewportWidth = 0;                     // Render target width per eye
    int viewportHeight = 0;                    // Render target height per eye
};
```

### Member Details

| Member | Type | Description |
|--------|------|-------------|
| `viewMatrix` | `XMFLOAT4X4` | The view transformation matrix for this eye. Accounts for head tracking position/orientation plus the eye offset (half IPD). |
| `projectionMatrix` | `XMFLOAT4X4` | The projection matrix for this eye. Uses the asymmetric frustum reported by the VR runtime for correct lens distortion. |
| `position` | `XMFLOAT3` | The world-space position of this eye. Differs from head position by the inter-pupillary distance (IPD) offset. |
| `viewportWidth` | `int` | Recommended render target width for this eye, as reported by the VR runtime. |
| `viewportHeight` | `int` | Recommended render target height for this eye. |

### Stereoscopic Rendering Pattern

```cpp
// Get per-eye render target dimensions
int width, height;
vr.GetRecommendedRenderSize(width, height);

// Create render targets (once, at initialization)
auto leftRT = CreateRenderTarget(width, height);
auto rightRT = CreateRenderTarget(width, height);

// Per frame
vr.UpdateTracking();

const auto& leftEye = vr.GetLeftEye();
const auto& rightEye = vr.GetRightEye();

// Render left eye
SetRenderTarget(leftRT);
SetViewport(0, 0, leftEye.viewportWidth, leftEye.viewportHeight);
RenderScene(leftEye.viewMatrix, leftEye.projectionMatrix);

// Render right eye
SetRenderTarget(rightRT);
SetViewport(0, 0, rightEye.viewportWidth, rightEye.viewportHeight);
RenderScene(rightEye.viewMatrix, rightEye.projectionMatrix);

// Submit both textures to the VR compositor
SubmitFrames(leftRT, rightRT);
```

---

## VRTrackingSpace Enum

```cpp
enum class VRTrackingSpace
{
    Seated,    // Seated or standing in a small area
    RoomScale  // Full room-scale tracking with boundary
};
```

| Value | Use Case | Origin | Description |
|-------|----------|--------|-------------|
| `Seated` | Cockpit games, seated experiences | Head position at launch / recenter | The origin is at the user's head when they start or recenter. Small positional tracking area. Suitable for gamepad-based games or cockpit simulators. |
| `RoomScale` | FPS, room-scale experiences | Floor center of play area | The origin is at floor level in the center of the user's configured play area. Full positional tracking within the boundary. Default value. |

```cpp
vr.SetTrackingSpace(VRTrackingSpace::RoomScale);

// Get the current tracking space
VRTrackingSpace space = vr.GetTrackingSpace();

// Recenter seated position (only meaningful for Seated mode)
vr.RecenterTracking();
```

---

## VRSystem API Reference

### Lifecycle

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `VRSystem()` | Construct a VR system instance. Does not initialize hardware. |
| Destructor | `~VRSystem()` | Destroy the instance. Calls `Shutdown()` if still initialized. |
| `Initialize` | `bool Initialize()` | Connect to the VR runtime and initialize hardware. Returns `true` if VR hardware was found and initialized. |
| `Shutdown` | `void Shutdown()` | Release all VR resources and disconnect from the runtime. |
| `IsAvailable` | `bool IsAvailable() const` | Check if VR is initialized and available. Returns `m_initialized`. |

### Tracking

| Method | Signature | Description |
|--------|-----------|-------------|
| `UpdateTracking` | `void UpdateTracking()` | Update all tracking data: head position/orientation, eye matrices, and controller states. Call once per frame before rendering. |
| `GetHeadPosition` | `DirectX::XMFLOAT3 GetHeadPosition() const` | Get the head position in world space. |
| `GetHeadOrientation` | `DirectX::XMFLOAT4 GetHeadOrientation() const` | Get the head orientation as a quaternion (x, y, z, w). |

### Eye Rendering

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetLeftEye` | `const VREye& GetLeftEye() const` | Get the left eye rendering data (view matrix, projection matrix, position, viewport). |
| `GetRightEye` | `const VREye& GetRightEye() const` | Get the right eye rendering data. |
| `GetRecommendedRenderSize` | `void GetRecommendedRenderSize(int& width, int& height) const` | Get the recommended per-eye render target resolution. Default values: 1440x1600. |

### Controllers

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetLeftController` | `const VRController& GetLeftController() const` | Get the left controller state. |
| `GetRightController` | `const VRController& GetRightController() const` | Get the right controller state. |
| `TriggerHaptic` | `void TriggerHaptic(bool isLeft, float amplitude, float duration)` | Trigger haptic feedback on a controller. `isLeft=true` for left controller, `false` for right. `amplitude` is 0.0 to 1.0. `duration` is in seconds. |

### Tracking Space

| Method | Signature | Description |
|--------|-----------|-------------|
| `SetTrackingSpace` | `void SetTrackingSpace(VRTrackingSpace space)` | Set the tracking space type (Seated or RoomScale). |
| `GetTrackingSpace` | `VRTrackingSpace GetTrackingSpace() const` | Get the current tracking space. |
| `RecenterTracking` | `void RecenterTracking()` | Reset the seated position origin to the current head position. Primarily useful in Seated mode. |

### Console Integration

| Method | Signature | Description |
|--------|-----------|-------------|
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Return a status string with initialization state, tracking space, head position, controller connectivity, and recommended render size. |

---

## Internal Members

| Member | Type | Default | Description |
|--------|------|---------|-------------|
| `m_initialized` | `bool` | `false` | Whether the VR system has been successfully initialized |
| `m_headPosition` | `DirectX::XMFLOAT3` | `{0, 0, 0}` | Current head position in world space |
| `m_headOrientation` | `DirectX::XMFLOAT4` | `{0, 0, 0, 1}` | Current head orientation quaternion (identity = looking forward) |
| `m_leftEye` | `VREye` | (default) | Left eye rendering data |
| `m_rightEye` | `VREye` | (default) | Right eye rendering data |
| `m_leftController` | `VRController` | (default, disconnected) | Left motion controller state |
| `m_rightController` | `VRController` | (default, disconnected) | Right motion controller state |
| `m_trackingSpace` | `VRTrackingSpace` | `RoomScale` | Current tracking space configuration |
| `m_recommendedWidth` | `int` | `1440` | Recommended per-eye render width |
| `m_recommendedHeight` | `int` | `1600` | Recommended per-eye render height |

---

## Platform Dependencies

The VR system depends on platform-specific types from `Core/Platform.h`:

```cpp
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
```

All position, orientation, and matrix types use DirectXMath (`XMFLOAT3`, `XMFLOAT4`, `XMFLOAT4X4`, `XMFLOAT2`). On Linux/macOS, these types are provided by the cross-platform stubs in `Core/Platform.h`.

---

## Haptic Feedback

Haptic feedback provides physical vibration in the motion controllers. Common use cases include weapon firing, collision feedback, UI interaction confirmation, and environmental effects.

```cpp
// Right controller, 50% intensity, 0.1 second burst
vr.TriggerHaptic(false, 0.5f, 0.1f);

// Left controller, full intensity, 0.3 second buzz
vr.TriggerHaptic(true, 1.0f, 0.3f);

// Light tap on right controller
vr.TriggerHaptic(false, 0.2f, 0.05f);
```

### Parameter Reference

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `isLeft` | `bool` | true/false | `true` = left controller, `false` = right controller |
| `amplitude` | `float` | 0.0 - 1.0 | Vibration intensity. 0.0 = off, 1.0 = maximum. |
| `duration` | `float` | seconds | Duration of the haptic pulse. Very short values (0.01-0.05s) create taps; longer values (0.1-0.5s) create buzzes. |

### Haptic Feedback Patterns

| Pattern | Amplitude | Duration | Use Case |
|---------|-----------|----------|----------|
| Light tap | 0.1 - 0.2 | 0.02 - 0.05s | UI button hover, soft contact |
| Button press | 0.3 - 0.5 | 0.05 - 0.1s | UI selection, object pickup |
| Impact | 0.6 - 0.8 | 0.1 - 0.2s | Weapon hit, collision, landing |
| Heavy impact | 0.9 - 1.0 | 0.2 - 0.4s | Explosion, heavy weapon fire |
| Continuous buzz | 0.2 - 0.4 | Per frame | Holding a vibrating object, engine rumble |

---

## Render Target Configuration

The VR runtime reports the optimal per-eye resolution based on the headset's native display resolution and any supersampling configured by the user.

```cpp
int width, height;
vr.GetRecommendedRenderSize(width, height);
// Default: 1440 x 1600 (typical for Quest 2 / Index-class headsets)

// Create per-eye render targets at the recommended size
auto leftRT = GraphicsEngine::CreateRenderTarget(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
auto rightRT = GraphicsEngine::CreateRenderTarget(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
```

### Resolution Scaling

For performance tuning, you can render at a fraction of the recommended resolution:

```cpp
float renderScale = 0.8f;  // 80% of recommended resolution
int scaledWidth = static_cast<int>(width * renderScale);
int scaledHeight = static_cast<int>(height * renderScale);
```

### Common Headset Resolutions

| Headset | Per-Eye Resolution | Refresh Rate | Recommended Width x Height |
|---------|-------------------|--------------|---------------------------|
| Meta Quest 2 | 1832 x 1920 | 72/90/120 Hz | ~1440 x 1600 |
| Meta Quest 3 | 2064 x 2208 | 90/120 Hz | ~1680 x 1760 |
| Valve Index | 1440 x 1600 | 80/90/120/144 Hz | 1440 x 1600 |
| HTC Vive Pro 2 | 2448 x 2448 | 90/120 Hz | ~2000 x 2000 |
| HP Reverb G2 | 2160 x 2160 | 90 Hz | ~2160 x 2160 |

---

## Motion Controllers: Detailed Usage

### Checking Connectivity

Always check `connected` before reading controller data. Controllers may disconnect during gameplay (battery loss, out of tracking range).

```cpp
const auto& right = vr.GetRightController();
if (!right.connected)
{
    // Show "controller disconnected" notification
    ShowNotification("Right controller lost. Please move it into tracking range.");
    return;
}
```

### Trigger and Grip Thresholds

Analog axes require thresholding to distinguish intentional presses from accidental touches:

```cpp
const float TRIGGER_THRESHOLD = 0.7f;
const float GRIP_THRESHOLD = 0.5f;

bool isFiring = right.triggerValue > TRIGGER_THRESHOLD;
bool isGripping = right.gripValue > GRIP_THRESHOLD;

// Thumbstick dead zone
const float DEADZONE = 0.15f;
float stickX = std::abs(right.thumbstick.x) > DEADZONE ? right.thumbstick.x : 0.0f;
float stickY = std::abs(right.thumbstick.y) > DEADZONE ? right.thumbstick.y : 0.0f;
```

### Using Velocity for Throw Physics

When the player releases a grabbed object, use the controller's velocity to calculate throw force:

```cpp
void OnObjectReleased(PhysicsBody& body)
{
    const auto& controller = vr.GetRightController();

    // Apply linear velocity
    DirectX::XMFLOAT3 throwVel = controller.velocity;

    // Scale up slightly for a more satisfying throw feel
    constexpr float THROW_SCALE = 1.5f;
    throwVel.x *= THROW_SCALE;
    throwVel.y *= THROW_SCALE;
    throwVel.z *= THROW_SCALE;

    body.SetLinearVelocity(throwVel);
    body.SetAngularVelocity(controller.angularVelocity);
}
```

---

## Tracking Space Configuration

### Seated Mode

Suitable for cockpit games, racing simulators, or experiences where the player remains seated. The origin is at the player's head position when `Initialize()` is called or `RecenterTracking()` is invoked.

```cpp
vr.SetTrackingSpace(VRTrackingSpace::Seated);

// Player presses "recenter" button
vr.RecenterTracking();
// Head position resets to (0, 0, 0) relative to the seated origin
```

### Room-Scale Mode

Suitable for FPS games, room-scale experiences, and any game where the player physically walks. The origin is at floor level in the center of the play area boundary.

```cpp
vr.SetTrackingSpace(VRTrackingSpace::RoomScale);

// Head position reports absolute position relative to the room center
auto headPos = vr.GetHeadPosition();
// headPos.y represents actual height above the floor
```

### Choosing a Tracking Space

| Mode | Origin | Y-Axis Zero | Best For |
|------|--------|-------------|----------|
| `Seated` | User's head at recenter time | Head height | Cockpit sims, seated games, gamepad VR |
| `RoomScale` | Floor center of play area | Floor level | FPS, action games, room-scale experiences |

---

## VR Rendering Pipeline Integration

### Integration with GraphicsEngine

The VR system integrates with the existing [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) pipeline. The key modification is rendering the scene twice (once per eye) with different view and projection matrices.

```
Standard Render Loop:          VR Render Loop:

1. UpdateTracking()            1. vr.UpdateTracking()
2. RenderScene(camera)         2. RenderScene(leftEye)
3. PostProcess                 3. PostProcess (left)
4. Present                     4. RenderScene(rightEye)
                               5. PostProcess (right)
                               6. SubmitToCompositor
```

### Post-Processing Considerations for VR

Some post-processing effects should be disabled or modified in VR:

| Effect | VR Recommendation | Reason |
|--------|-------------------|--------|
| Motion blur | **Disable** | The VR compositor handles temporal prediction. Motion blur causes severe nausea. |
| Depth of field | **Disable** | Conflicts with natural eye accommodation. The user's eyes perform their own focus. |
| Chromatic aberration | **Disable** | HMD lenses already introduce chromatic aberration that the runtime corrects. |
| Film grain | **Disable** | Appears unnatural at close viewing distance in VR. |
| Bloom | Reduce intensity | Can be distracting at VR close viewing distance. Use subtle amounts. |
| Tonemapping | Use ACES | Standard tonemapping works well in VR. |
| SSAO | Optional | Expensive at VR resolution. Consider baked ambient occlusion instead. |
| Anti-aliasing | MSAA recommended | TAA can cause ghosting with head motion. MSAA 4x is preferred. |

---

## Performance Targets for VR

VR requires consistently hitting the headset's refresh rate. Dropped frames cause judder and motion sickness.

| Refresh Rate | Frame Budget | GPU Recommendation |
|-------------|-------------|-------------------|
| 72 Hz | 13.9 ms | GTX 1070 / RX 5600 XT |
| 90 Hz | 11.1 ms | RTX 2070 / RX 6700 XT |
| 120 Hz | 8.3 ms | RTX 3080 / RX 6800 XT |
| 144 Hz | 6.9 ms | RTX 4080 / RX 7900 XT |

### Performance Tips

1. **Use the recommended render size.** Do not oversample beyond what the runtime recommends.
2. **Reduce shadow quality.** Use 2 shadow cascades instead of 4. Lower shadow map resolution to 1024.
3. **Aggressive LOD.** Increase LOD bias to push objects to lower detail levels earlier.
4. **Limit draw calls.** Use instancing and batching aggressively. Target under 1500 draw calls.
5. **Disable expensive post-processing.** See the table above.
6. **Use MSAA instead of TAA.** 4x MSAA avoids the temporal ghosting artifacts that TAA produces with VR head motion.

---

## Framework Status and Implementation Notes

The current VR system is a **framework stub**. The public API and data structures are fully defined, but the actual OpenXR integration requires:

1. **OpenXR SDK:** The OpenXR loader and headers must be available at build time when `ENABLE_VR=ON`.
2. **VR Runtime:** A compatible runtime must be installed (SteamVR, Meta OpenXR, Windows Mixed Reality OpenXR).
3. **Graphics Binding:** The `Initialize()` method needs to create the OpenXR graphics binding for D3D11 (using `XR_KHR_D3D11_enable`).
4. **Frame Loop Integration:** The `UpdateTracking()` method needs to call `xrWaitFrame`, `xrBeginFrame`, and the rendering code needs to call `xrEndFrame`.

The stub provides default values for all tracking data (zero positions, identity orientations, default render size) so that VR-dependent code can be developed and tested without VR hardware.

---

## Console Commands

| Command | Description |
|---------|-------------|
| `vr_status` | Show VR system status including: initialized state, tracking space, head position, head orientation, left/right controller connectivity, and recommended render size. Calls `Console_GetStatus()` internally. |

### Example Console Output

```
> vr_status
VR System Status:
  Initialized: true
  Tracking Space: RoomScale
  Head Position: (0.12, 1.65, -0.34)
  Head Orientation: (0.00, 0.02, 0.00, 1.00)
  Left Controller: Connected
    Position: (-0.25, 1.10, -0.40)
    Trigger: 0.00  Grip: 0.00
  Right Controller: Connected
    Position: (0.30, 1.05, -0.35)
    Trigger: 0.85  Grip: 0.12
  Recommended Render Size: 1440 x 1600
```

---

## Complete Integration Example

```cpp
#include "Engine/VR/VRSystem.h"
#include "Graphics/GraphicsEngine.h"

using namespace Spark::VR;

class VRGameApp
{
public:
    bool Initialize()
    {
        m_vr = std::make_unique<VRSystem>();
        if (!m_vr->Initialize())
        {
            LOG_WARN("VR hardware not found, falling back to flat-screen mode");
            m_vrEnabled = false;
            return true;  // Continue without VR
        }

        m_vrEnabled = true;
        m_vr->SetTrackingSpace(VRTrackingSpace::RoomScale);

        // Create per-eye render targets
        int width, height;
        m_vr->GetRecommendedRenderSize(width, height);
        m_leftEyeRT = CreateRenderTarget(width, height);
        m_rightEyeRT = CreateRenderTarget(width, height);

        return true;
    }

    void Update(float deltaTime)
    {
        if (!m_vrEnabled) return;

        m_vr->UpdateTracking();

        // Use head position for camera
        auto headPos = m_vr->GetHeadPosition();
        auto headOri = m_vr->GetHeadOrientation();
        UpdateCamera(headPos, headOri);

        // Process controller input
        ProcessInput();
    }

    void Render()
    {
        if (!m_vrEnabled)
        {
            RenderFlatScreen();
            return;
        }

        // Render left eye
        const auto& leftEye = m_vr->GetLeftEye();
        SetRenderTarget(m_leftEyeRT);
        RenderScene(leftEye.viewMatrix, leftEye.projectionMatrix);

        // Render right eye
        const auto& rightEye = m_vr->GetRightEye();
        SetRenderTarget(m_rightEyeRT);
        RenderScene(rightEye.viewMatrix, rightEye.projectionMatrix);
    }

    void ProcessInput()
    {
        const auto& right = m_vr->GetRightController();
        if (!right.connected) return;

        // Fire weapon on trigger press
        if (right.triggerValue > 0.8f)
        {
            FireWeapon(right.position, right.orientation);
            m_vr->TriggerHaptic(false, 0.6f, 0.1f);  // Recoil feedback
        }

        // Grab objects on grip
        if (right.gripValue > 0.5f)
        {
            TryGrabObject(right.position);
        }

        // Movement on left thumbstick
        const auto& left = m_vr->GetLeftController();
        if (left.connected)
        {
            float moveX = left.thumbstick.x;
            float moveY = left.thumbstick.y;
            MovePlayer(moveX, moveY);
        }
    }

    void Shutdown()
    {
        if (m_vr)
        {
            m_vr->Shutdown();
        }
    }

private:
    std::unique_ptr<VRSystem> m_vr;
    bool m_vrEnabled = false;
    RenderTarget m_leftEyeRT;
    RenderTarget m_rightEyeRT;
};
```

---

## Troubleshooting

### Initialize() returns false

1. Verify that a VR runtime is installed and running (SteamVR, Oculus app, WMR Portal).
2. Verify that the headset is connected and powered on.
3. Ensure `ENABLE_VR=ON` was set during CMake configuration.
4. Check that the OpenXR SDK libraries are available at build time.
5. Check the engine log for specific initialization error messages.

### Tracking data is all zeros

If `GetHeadPosition()` returns `{0, 0, 0}` and orientation is identity, ensure:

1. `UpdateTracking()` is being called each frame.
2. `Initialize()` returned `true`.
3. The headset is being worn or placed within tracking range.
4. The tracking space is calibrated in the VR runtime settings.

### Controllers show connected=false

1. Ensure controllers are powered on and paired with the headset.
2. Ensure controllers are within tracking range (visible to sensors/cameras).
3. Check battery levels.
4. Some runtimes require a "wake up" gesture (pressing a button).

### Poor performance / dropped frames

1. Check GPU frame time. VR requires rendering the scene twice at high resolution.
2. Reduce shadow quality, disable SSAO, and lower LOD bias.
3. Ensure MSAA is used instead of TAA (TAA ghosting is worse in VR).
4. Disable motion blur, depth of field, and chromatic aberration.
5. Use `vr_status` in the console to verify recommended render size is not excessively high.

### Haptic feedback not working

1. Verify the controller is connected.
2. Ensure `amplitude` is greater than 0.0 and `duration` is greater than 0.0.
3. Some runtimes have a minimum haptic duration; try at least 0.01s.
4. Check that the VR runtime supports haptics for the connected controller model.

---

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| `Initialize()` called when no VR hardware present | Returns `false`. `IsAvailable()` returns `false`. All tracking data returns defaults. |
| `UpdateTracking()` called before `Initialize()` | No-op. Tracking data remains at default values. |
| `TriggerHaptic()` called when VR not initialized | No-op. No crash. |
| `Shutdown()` called when not initialized | Safe no-op. |
| `Shutdown()` called multiple times | Safe; sets `m_initialized = false` on first call. |
| Controller disconnects mid-frame | `connected` field becomes `false` on next `UpdateTracking()`. Other fields retain last known values. |
| Headset removed during gameplay | Runtime signals session state change. Tracking data may freeze at last known values. |
| `RecenterTracking()` in RoomScale mode | Resets the origin for seated-style recenter. Most useful in Seated mode. |

---

## CMake Configuration

To enable VR support in the build:

```bash
cmake -B build -DENABLE_VR=ON

# Or with a preset
cmake --preset windows-release -DENABLE_VR=ON
```

When `ENABLE_VR=OFF` (default), the VR source files are excluded from the build and all VR-related code paths are compiled out via preprocessor guards.

---

## See Also

- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Stereoscopic rendering pipeline, render target management
- [Input System](../subsystems/Input-System.md) -- Standard controller input alongside VR input
- [Physics](../subsystems/Physics.md) -- VR hand interaction with physics objects, rigid body constraints
- [Entity-Component-System](../subsystems/Entity-Component-System.md) -- Attaching VR tracking to entities
- [UI System](../subsystems/UI-System.md) -- World-space UI rendering for VR menus
- [Audio](../subsystems/Audio.md) -- Spatial audio is critical for VR immersion (3D sound positioning)
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Editor VR preview mode
- [Build System](Build-System) -- CMake toggles for VR support
