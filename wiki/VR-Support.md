# VR Support

SparkEngine provides a VR/AR integration framework designed for OpenXR. It handles head tracking, stereoscopic rendering, motion controller input, and haptic feedback. Currently a framework stub requiring the OpenXR SDK and a VR runtime (SteamVR, Oculus, WMR).

**Source:** `SparkEngine/Source/Engine/VR/VRSystem.h`

**CMake toggle:** `ENABLE_VR=ON`

## Overview

| Class | Responsibility |
|-------|---------------|
| `VRSystem` | Hardware initialization, tracking updates, rendering data |
| `VRController` | State of a motion controller (position, buttons, triggers) |
| `VREye` | Per-eye view/projection matrices and render target size |

## Quick Start

```cpp
VRSystem vr;
if (vr.Initialize()) {
    vr.SetTrackingSpace(VRTrackingSpace::RoomScale);

    // Per frame:
    vr.UpdateTracking();

    auto headPos = vr.GetHeadPosition();
    auto headOri = vr.GetHeadOrientation();

    // Render left eye
    const auto& leftEye = vr.GetLeftEye();
    RenderScene(leftEye.viewMatrix, leftEye.projectionMatrix);

    // Render right eye
    const auto& rightEye = vr.GetRightEye();
    RenderScene(rightEye.viewMatrix, rightEye.projectionMatrix);
}
```

## Motion Controllers

```cpp
const auto& left = vr.GetLeftController();
const auto& right = vr.GetRightController();

if (right.connected) {
    auto pos = right.position;
    float trigger = right.triggerValue;      // 0-1
    float grip = right.gripValue;            // 0-1
    auto stick = right.thumbstick;           // x,y: -1 to 1
    uint32_t buttons = right.buttonMask;
}

// Haptic feedback
vr.TriggerHaptic(false, 0.5f, 0.1f);  // Right controller, 50% amplitude, 0.1s
```

## Tracking Space

```cpp
enum class VRTrackingSpace {
    Seated,   // Seated or standing (small area)
    RoomScale // Full room-scale tracking
};

vr.SetTrackingSpace(VRTrackingSpace::RoomScale);
vr.RecenterTracking();  // Reset seated position
```

## Render Target Size

```cpp
int width, height;
vr.GetRecommendedRenderSize(width, height);
// Create per-eye render targets at recommended resolution
```

## Console Commands

```
vr_status    # Show VR system status and tracking info
```

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — Stereoscopic rendering pipeline
- [Input System](Input-System) — Controller input alongside VR input
- [Physics](Physics) — VR hand interaction with physics objects
