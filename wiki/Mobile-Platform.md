# Mobile Platform

SparkEngine provides a mobile platform abstraction for iOS and Android, handling touch input, gesture recognition, GPU quality presets, battery-aware performance scaling, and screen orientation management.

**Source:** `SparkEngine/Source/Engine/Mobile/MobilePlatform.h`

**CMake toggle:** `ENABLE_MOBILE=ON`

## Overview

| Class | Responsibility |
|-------|---------------|
| `MobilePlatform` | Touch input, gestures, quality settings, battery, orientation |
| `TouchEvent` | Raw touch point data (position, pressure, type) |
| `Gesture` | Recognized gesture (tap, swipe, pinch, rotate) |
| `MobileQualitySettings` | GPU quality parameters derived from preset |

## Quick Start

```cpp
MobilePlatform mobile;
mobile.Initialize();
mobile.SetQualityPreset(MobileQualityPreset::Auto);
mobile.SetOrientation(ScreenOrientation::LandscapeLeft);

// Per frame:
mobile.Update(deltaTime);

auto gestures = mobile.GetGestures();
for (const auto& g : gestures) {
    if (g.type == GestureType::Tap) { HandleTap(g.x, g.y); }
    if (g.type == GestureType::Pinch) { HandleZoom(g.scale); }
}
```

## Touch Input

```cpp
// Process raw touch events from the OS
mobile.ProcessTouchEvent(touchEvent);

// Query active touches
auto touches = mobile.GetActiveTouches();

// Register gesture callbacks
mobile.OnGesture(GestureType::Swipe, [](const Gesture& g) {
    MoveCamera(g.deltaX, g.deltaY);
});
```

## Gesture Types

```cpp
enum class GestureType {
    Tap, DoubleTap, LongPress, Swipe, Pinch, Rotate
};
```

## Quality Presets

```cpp
enum class MobileQualityPreset { Low, Medium, High, Auto };

mobile.SetQualityPreset(MobileQualityPreset::Medium);
const auto& settings = mobile.GetQualitySettings();
// settings.renderScale, settings.shadowResolution, settings.maxDrawCalls, etc.
```

| Setting | Low | Medium | High |
|---------|-----|--------|------|
| Render scale | 0.5 | 0.75 | 1.0 |
| Shadow resolution | 256 | 512 | 1024 |
| Post-processing | Off | On | On |
| SSAO | Off | Off | On |

## Battery-Aware Scaling

```cpp
mobile.SetBatteryAwareScaling(true);
float battery = mobile.GetBatteryLevel();  // 0.0-1.0, -1 if unknown
bool charging = mobile.IsCharging();
```

When enabled, quality is automatically reduced as battery drops below thresholds.

## Screen Orientation and Safe Area

```cpp
mobile.SetOrientation(ScreenOrientation::Auto);
const SafeArea& safe = mobile.GetSafeArea();
// safe.top, safe.bottom, safe.left, safe.right (insets for notches)
```

## Console Commands

```
mobile_status    # Show mobile platform status
```

---

## See Also

- [Input System](Input-System) — Keyboard/mouse input on desktop
- [Rendering and Graphics](Rendering-and-Graphics) — Quality settings and render scale
- [UI System](UI-System) — Touch-friendly UI layout
