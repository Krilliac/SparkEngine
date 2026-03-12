# Mobile Platform

SparkEngine provides a mobile platform abstraction for iOS and Android, handling touch input, gesture recognition, GPU quality presets, battery-aware performance scaling, and screen orientation management.

**Source:** `SparkEngine/Source/Engine/Mobile/MobilePlatform.h`, `SparkEngine/Source/Engine/Mobile/MobilePlatform.cpp`

**CMake toggle:** `ENABLE_MOBILE=ON`

---

## Architecture Overview

```
+---------------------------------------------+
|           OS Touch Events (iOS/Android)      |
+---------------------+-----------------------+
                      |
                      v
+---------------------+-----------------------+
|              MobilePlatform                  |
|  +---------------+  +-------------------+   |
|  | Touch Tracker |  | Gesture Recognizer|   |
|  | (active       |  | (tap, swipe,      |   |
|  |  touches)     |  |  pinch, rotate)   |   |
|  +-------+-------+  +--------+----------+   |
|          |                    |              |
|  +-------v--------------------v----------+  |
|  |       Gesture Callback Dispatch       |  |
|  +---------------------------------------+  |
|                                              |
|  +-------------------+  +----------------+  |
|  | Quality Presets    |  | Battery-Aware  |  |
|  | (Low/Med/High/    |  | Scaling        |  |
|  |  Auto)            |  | (auto-downgrade|  |
|  +---------+---------+  |  on low batt)  |  |
|            |             +--------+-------+  |
|            +------+------+--------+          |
|                   |                          |
|  +----------------v-------------------------+|
|  | Screen Orientation + Safe Area Management||
|  +------------------------------------------+|
+---------------------------------------------+
                      |
       Outputs to: Rendering, Input, UI Systems
```

---

## Overview

| Class                  | Responsibility                                                |
|------------------------|---------------------------------------------------------------|
| `MobilePlatform`       | Touch input, gestures, quality settings, battery, orientation |
| `TouchEvent`           | Raw touch point data (position, pressure, type)               |
| `Gesture`              | Recognized gesture (tap, swipe, pinch, rotate)                |
| `MobileQualitySettings`| GPU quality parameters derived from preset                    |

All types live in the `Spark::Mobile` namespace.

---

## Enabling in CMake

```cmake
set(ENABLE_MOBILE ON CACHE BOOL "Enable mobile platform support")
```

When `ENABLE_MOBILE` is `OFF` (the default for desktop builds), none of the mobile platform code is compiled. This ensures zero overhead for Windows/Linux builds.

---

## Quick Start

```cpp
#include "Engine/Mobile/MobilePlatform.h"

Spark::Mobile::MobilePlatform mobile;
mobile.Initialize();
mobile.SetQualityPreset(Spark::Mobile::MobileQualityPreset::Auto);
mobile.SetOrientation(Spark::Mobile::ScreenOrientation::LandscapeLeft);

// Per frame:
mobile.Update(deltaTime);

auto gestures = mobile.GetGestures();
for (const auto& g : gestures)
{
    if (g.type == Spark::Mobile::GestureType::Tap)
    {
        HandleTap(g.x, g.y);
    }
    if (g.type == Spark::Mobile::GestureType::Pinch)
    {
        HandleZoom(g.scale);
    }
}
```

---

## Touch Input

### `TouchEventType` Enum

Describes the lifecycle phase of a single touch point:

```cpp
enum class TouchEventType
{
    Began,      // Finger first touches screen
    Moved,      // Finger moves while touching
    Ended,      // Finger lifts off screen
    Cancelled   // Touch interrupted by system (e.g., incoming call)
};
```

| Value       | Description                                          | Action Taken                        |
|-------------|------------------------------------------------------|-------------------------------------|
| `Began`     | New finger contact detected                          | Added to active touches list        |
| `Moved`     | Existing touch position changed                      | Updates x, y, pressure in list      |
| `Ended`     | Finger lifted from screen                            | Removed from active touches list    |
| `Cancelled` | OS interrupted the touch (call, notification, etc.)  | Removed from active touches list    |

### `TouchEvent` Struct

A single raw touch point event from the operating system:

```cpp
struct TouchEvent
{
    uint32_t touchId = 0;         // Unique touch identifier
    TouchEventType type = TouchEventType::Began;
    float x = 0.0f;              // Screen X (0-1 normalized)
    float y = 0.0f;              // Screen Y (0-1 normalized)
    float pressure = 1.0f;       // Touch pressure (0-1)
    float timestamp = 0.0f;      // Event timestamp
};
```

| Field       | Type            | Default                | Description                         |
|-------------|-----------------|------------------------|-------------------------------------|
| `touchId`   | `uint32_t`      | `0`                    | Unique per-finger ID across a touch session |
| `type`      | `TouchEventType`| `Began`                | Phase of the touch event            |
| `x`         | `float`         | `0.0f`                 | Normalized screen X coordinate [0, 1] |
| `y`         | `float`         | `0.0f`                 | Normalized screen Y coordinate [0, 1] |
| `pressure`  | `float`         | `1.0f`                 | Pressure sensitivity [0, 1] (1.0 on devices without pressure support) |
| `timestamp` | `float`         | `0.0f`                 | Time of the touch event in seconds  |

### Processing Touch Events

Raw touch events are forwarded from the OS-specific platform layer to the `MobilePlatform`:

```cpp
// Called by the platform-specific layer (iOS UITouch handler, Android MotionEvent)
void ProcessTouchEvent(const TouchEvent& event);
```

The internal touch tracker maintains a list of active touches. The lifecycle is:

1. **Began**: A new `TouchEvent` is appended to the active touches list.
2. **Moved**: The existing touch with the matching `touchId` is updated (x, y, pressure).
3. **Ended / Cancelled**: The touch with the matching `touchId` is removed from the list.

### Querying Active Touches

```cpp
// Get a snapshot of all currently active touch points
std::vector<TouchEvent> GetActiveTouches() const;
```

This returns a copy of the internal touch list. The returned vector contains one entry per active finger on the screen.

---

## Gesture Recognition

### `GestureType` Enum

```cpp
enum class GestureType
{
    Tap,        // Single finger tap
    DoubleTap,  // Two taps in quick succession
    LongPress,  // Finger held stationary for threshold duration
    Swipe,      // Finger moved quickly across the screen
    Pinch,      // Two fingers moving together/apart
    Rotate      // Two fingers rotating around a center point
};
```

### `Gesture` Struct

A recognized touch gesture with all relevant parameters:

```cpp
struct Gesture
{
    GestureType type = GestureType::Tap;
    float x = 0.0f, y = 0.0f;           // Gesture center (normalized)
    float deltaX = 0.0f, deltaY = 0.0f; // Movement delta (swipe)
    float scale = 1.0f;                 // Scale factor (pinch)
    float rotation = 0.0f;              // Rotation angle in radians (rotate)
    int touchCount = 1;                 // Number of fingers involved
};
```

| Field        | Type           | Used By        | Description                                  |
|--------------|----------------|----------------|----------------------------------------------|
| `type`       | `GestureType`  | All            | What gesture was recognized                  |
| `x`, `y`     | `float`        | All            | Center point of the gesture (normalized 0-1) |
| `deltaX`     | `float`        | Swipe          | Horizontal swipe distance (normalized)       |
| `deltaY`     | `float`        | Swipe          | Vertical swipe distance (normalized)         |
| `scale`      | `float`        | Pinch          | Scale factor (>1 = spread apart, <1 = pinch together) |
| `rotation`   | `float`        | Rotate         | Rotation angle in radians                    |
| `touchCount` | `int`          | All            | Number of fingers involved (1, 2, etc.)      |

### Retrieving Gestures

Gestures recognized since the last frame are available via:

```cpp
std::vector<Gesture> GetGestures() const;
```

The gesture list is cleared at the start of each `Update()` call, so gestures are only valid for one frame.

### Gesture Callbacks

For event-driven gesture handling, register callbacks per gesture type:

```cpp
void OnGesture(GestureType type, std::function<void(const Gesture&)> callback);
```

Multiple callbacks can be registered for the same gesture type. Callbacks are invoked during `Update()` when gestures are recognized.

```cpp
// Register gesture callbacks
mobile.OnGesture(GestureType::Tap, [](const Gesture& g) {
    // Handle tap at normalized (g.x, g.y)
    SelectObjectAt(g.x, g.y);
});

mobile.OnGesture(GestureType::Swipe, [](const Gesture& g) {
    // Move camera based on swipe direction
    MoveCamera(g.deltaX, g.deltaY);
});

mobile.OnGesture(GestureType::Pinch, [](const Gesture& g) {
    // Zoom camera based on pinch scale
    ZoomCamera(g.scale);
});

mobile.OnGesture(GestureType::Rotate, [](const Gesture& g) {
    // Rotate selected object
    RotateSelection(g.rotation);
});

mobile.OnGesture(GestureType::LongPress, [](const Gesture& g) {
    // Show context menu
    ShowContextMenu(g.x, g.y);
});

mobile.OnGesture(GestureType::DoubleTap, [](const Gesture& g) {
    // Toggle zoom
    ToggleZoom(g.x, g.y);
});
```

### Typical FPS Game Gesture Mapping

| Gesture    | FPS Action                              |
|------------|-----------------------------------------|
| Tap        | Fire weapon                             |
| DoubleTap  | Aim down sights toggle                  |
| LongPress  | Pick up item / interact                 |
| Swipe      | Look around (camera rotation)           |
| Pinch      | Zoom scope / adjust FOV                 |
| Rotate     | Rotate inventory item (menu context)    |

---

## Quality Presets

### `MobileQualityPreset` Enum

```cpp
enum class MobileQualityPreset
{
    Low,    // Lowest quality, maximum performance
    Medium, // Balanced quality and performance
    High,   // Highest quality for flagship devices
    Auto    // Auto-detect based on device capabilities
};
```

### `MobileQualitySettings` Struct

Quality settings derived from the active preset:

```cpp
struct MobileQualitySettings
{
    float renderScale = 1.0f;          // Render resolution scale (0.5 - 1.0)
    int shadowResolution = 512;        // Shadow map resolution (px)
    bool enablePostProcessing = true;  // Post-processing effects
    bool enableParticles = true;       // Particle effects
    int maxDrawCalls = 500;            // Maximum draw calls per frame
    int textureQuality = 2;            // 0=low, 1=medium, 2=high
    bool enableSSAO = false;           // Screen-space ambient occlusion
    float lodBias = 0.0f;             // LOD bias (positive = lower quality sooner)
};
```

### Preset Comparison Table

| Setting              | Low     | Medium  | High    | Auto (defaults to High) |
|----------------------|---------|---------|---------|-------------------------|
| `renderScale`        | 0.5     | 0.75    | 1.0     | 1.0                     |
| `shadowResolution`   | 256     | 512     | 1024    | 1024                    |
| `enablePostProcessing`| `false`| `true`  | `true`  | `true`                  |
| `enableParticles`    | `false` | `true`  | `true`  | `true`                  |
| `maxDrawCalls`       | 200     | 400     | 600     | 600                     |
| `textureQuality`     | 0       | 1       | 2       | 2                       |
| `enableSSAO`         | `false` | `false` | `true`  | `true`                  |
| `lodBias`            | 2.0     | 1.0     | 0.0     | 0.0                     |

### Setting and Querying Quality

```cpp
// Set preset
mobile.SetQualityPreset(MobileQualityPreset::Medium);

// Query current preset
MobileQualityPreset current = mobile.GetQualityPreset();

// Get derived settings for rendering configuration
const MobileQualitySettings& settings = mobile.GetQualitySettings();

// Use in rendering setup
renderer.SetResolutionScale(settings.renderScale);
renderer.SetShadowMapResolution(settings.shadowResolution);
renderer.SetPostProcessingEnabled(settings.enablePostProcessing);
renderer.SetMaxDrawCalls(settings.maxDrawCalls);
renderer.SetSSAOEnabled(settings.enableSSAO);
renderer.SetLODBias(settings.lodBias);
```

### Understanding LOD Bias

The `lodBias` field controls how aggressively the level-of-detail system selects lower-detail meshes:

| `lodBias` | Effect                                                     |
|-----------|------------------------------------------------------------|
| 0.0       | Standard LOD selection (highest quality at given distance) |
| 1.0       | Select one LOD level lower than normal                     |
| 2.0       | Select two LOD levels lower (most aggressive)              |

Higher values significantly reduce vertex throughput on low-end GPUs at the cost of visual quality.

---

## Battery-Aware Performance Scaling

### Overview

When battery-aware scaling is enabled, the system monitors battery level and automatically downgrades quality presets to extend play time. This only takes effect when the device is not charging.

```cpp
mobile.SetBatteryAwareScaling(true);  // Enable (default: true)
float battery = mobile.GetBatteryLevel();  // 0.0 - 1.0, or -1 if unknown
bool charging = mobile.IsCharging();
```

### Automatic Downgrade Thresholds

The automatic quality adjustment follows these rules (evaluated every frame in `Update()`):

| Battery Level  | Current Preset | Action                    |
|----------------|----------------|---------------------------|
| Below 10%      | Any except Low | Downgrade to **Low**      |
| Below 25%      | High           | Downgrade to **Medium**   |
| 25% or above   | Any            | No change                 |
| Charging       | Any            | No change (scaling disabled while charging) |

```
Battery Level
100% |=============================================|
     | Normal operation -- no quality changes      |
 25% |---------------------------------------------|
     | High -> Medium downgrade                    |
 10% |---------------------------------------------|
     | Any -> Low downgrade                        |
  0% |=============================================|
```

### Battery API Details

```cpp
// Returns battery level as a float:
//   0.0  = empty
//   1.0  = fully charged
//   -1.0 = unknown (e.g., desktop simulator, unsupported device)
float GetBatteryLevel() const;

// Returns true if the device is plugged in and charging
bool IsCharging() const;

// Enable or disable automatic quality scaling based on battery
void SetBatteryAwareScaling(bool enabled);
```

When `GetBatteryLevel()` returns `-1.0f`, the battery-aware scaling logic is skipped entirely, since the battery state is unknown.

---

## Screen Orientation and Safe Area

### `ScreenOrientation` Enum

```cpp
enum class ScreenOrientation
{
    Portrait,           // Home button at bottom
    PortraitUpsideDown, // Home button at top
    LandscapeLeft,      // Home button on left
    LandscapeRight,     // Home button on right
    Auto                // Rotate with device
};
```

### Setting Orientation

```cpp
mobile.SetOrientation(ScreenOrientation::LandscapeLeft);

// Query current orientation
ScreenOrientation current = mobile.GetOrientation();
```

For most FPS games, `LandscapeLeft` or `LandscapeRight` is preferred. Use `Auto` for menu-heavy or casual games that support rotation.

### `SafeArea` Struct

Modern mobile devices have notches, camera cutouts, and home indicators that reduce the usable screen area. The `SafeArea` provides insets in normalized coordinates:

```cpp
struct SafeArea
{
    float top = 0.0f;    // Inset from top edge (e.g., notch)
    float bottom = 0.0f; // Inset from bottom edge (e.g., home indicator)
    float left = 0.0f;   // Inset from left edge
    float right = 0.0f;  // Inset from right edge
};
```

```
+--------------------------------------------------+
|  top inset (notch/status bar)                    |
+------+----------------------------------+--------+
|      |                                  |        |
| left |        SAFE AREA                 | right  |
|      |     (usable screen space)        |        |
|      |                                  |        |
+------+----------------------------------+--------+
|  bottom inset (home indicator)                   |
+--------------------------------------------------+
```

### Using Safe Area for UI Layout

```cpp
const SafeArea& safe = mobile.GetSafeArea();

// Position HUD elements within the safe area
float hudLeft   = safe.left;
float hudRight  = 1.0f - safe.right;
float hudTop    = safe.top;
float hudBottom = 1.0f - safe.bottom;

// Example: position health bar in bottom-left safe corner
healthBar.SetPosition(hudLeft + 0.02f, hudBottom - 0.05f);

// Example: position minimap in top-right safe corner
minimap.SetPosition(hudRight - 0.12f, hudTop + 0.02f);
```

---

## `MobilePlatform` Full API Reference

```cpp
namespace Spark::Mobile
{
    class MobilePlatform
    {
    public:
        MobilePlatform();
        ~MobilePlatform() = default;

        // --- Lifecycle ---
        bool Initialize();
        void Update(float deltaTime);

        // --- Touch input ---
        void ProcessTouchEvent(const TouchEvent& event);
        std::vector<TouchEvent> GetActiveTouches() const;
        std::vector<Gesture> GetGestures() const;
        void OnGesture(GestureType type, std::function<void(const Gesture&)> callback);

        // --- Quality ---
        void SetQualityPreset(MobileQualityPreset preset);
        const MobileQualitySettings& GetQualitySettings() const;
        MobileQualityPreset GetQualityPreset() const;

        // --- Battery ---
        float GetBatteryLevel() const;
        bool IsCharging() const;
        void SetBatteryAwareScaling(bool enabled);

        // --- Screen ---
        void SetOrientation(ScreenOrientation orientation);
        ScreenOrientation GetOrientation() const;
        const SafeArea& GetSafeArea() const;

        // --- Console ---
        std::string Console_GetStatus() const;

    private:
        void RecognizeGestures();
        MobileQualitySettings GetSettingsForPreset(MobileQualityPreset preset) const;

        std::vector<TouchEvent> m_activeTouches;
        std::vector<Gesture> m_pendingGestures;
        std::unordered_map<int, std::vector<std::function<void(const Gesture&)>>> m_gestureCallbacks;

        MobileQualityPreset m_qualityPreset = MobileQualityPreset::Auto;
        MobileQualitySettings m_qualitySettings;
        ScreenOrientation m_orientation = ScreenOrientation::Auto;
        SafeArea m_safeArea;

        float m_batteryLevel = -1.0f;
        bool m_isCharging = false;
        bool m_batteryAwareScaling = true;
        bool m_initialized = false;
    };
}
```

### Member Variables

| Member                | Type                  | Default              | Description                                |
|-----------------------|-----------------------|----------------------|--------------------------------------------|
| `m_activeTouches`     | `vector<TouchEvent>`  | empty                | Currently active touch points              |
| `m_pendingGestures`   | `vector<Gesture>`     | empty                | Gestures recognized this frame             |
| `m_gestureCallbacks`  | `unordered_map`       | empty                | Registered gesture callbacks by type       |
| `m_qualityPreset`     | `MobileQualityPreset` | `Auto`               | Active quality preset                      |
| `m_qualitySettings`   | `MobileQualitySettings`| (from preset)       | Derived quality settings                   |
| `m_orientation`       | `ScreenOrientation`   | `Auto`               | Preferred screen orientation               |
| `m_safeArea`          | `SafeArea`            | all zeros            | Screen safe area insets                    |
| `m_batteryLevel`      | `float`               | `-1.0f`              | Battery level (0-1, -1 = unknown)          |
| `m_isCharging`        | `bool`                | `false`              | Whether device is charging                 |
| `m_batteryAwareScaling`| `bool`               | `true`               | Whether auto-scaling is enabled            |
| `m_initialized`       | `bool`                | `false`              | Whether Initialize() has been called       |

---

## Console Integration

The `Console_GetStatus()` method returns a formatted string for the in-game console:

```
=== Mobile Platform ===
Initialized: YES
Quality: Medium
Render scale: 0.75
Shadow res: 512
Active touches: 2
Battery: 65%
Charging: NO
Battery scaling: ON
```

### Console Command

| Command          | Description                      |
|------------------|----------------------------------|
| `mobile_status`  | Show mobile platform status      |

---

## Integration Patterns

### With the Input System

The mobile platform complements the desktop [Input System](Input-System). A typical setup routes touch events through the mobile platform while keyboard/mouse events go through the standard input system:

```cpp
#if defined(SPARK_PLATFORM_IOS) || defined(SPARK_PLATFORM_ANDROID)
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();
    // Route OS touch events to mobile platform
#else
    // Use standard keyboard/mouse input
    inputSystem.Initialize();
#endif
```

### With the Rendering System

Quality settings from the mobile platform should be applied to the [Rendering and Graphics](Rendering-and-Graphics) system at initialization and whenever the preset changes:

```cpp
void ApplyMobileQuality(const MobileQualitySettings& settings)
{
    auto& renderer = EngineContext::Get<GraphicsEngine>();
    renderer.SetResolutionScale(settings.renderScale);
    renderer.SetShadowResolution(settings.shadowResolution);
    renderer.SetPostProcessingEnabled(settings.enablePostProcessing);
    renderer.SetParticlesEnabled(settings.enableParticles);
    renderer.SetMaxDrawCalls(settings.maxDrawCalls);
    renderer.SetTextureQuality(settings.textureQuality);
    renderer.SetSSAOEnabled(settings.enableSSAO);
    renderer.SetLODBias(settings.lodBias);
}
```

### With the UI System

Use safe area insets from the mobile platform to ensure [UI elements](UI-System) are not obscured by hardware features:

```cpp
void LayoutMobileUI(const MobilePlatform& mobile)
{
    const auto& safe = mobile.GetSafeArea();

    // Offset all root UI containers by the safe area
    uiRoot.SetMargins(safe.left, safe.top, safe.right, safe.bottom);
}
```

---

## Performance Considerations

- **Touch processing**: `ProcessTouchEvent()` uses a linear scan of the active touches list. With typical mobile multi-touch (up to 10 simultaneous touches), this is negligible.
- **Gesture recognition**: `RecognizeGestures()` is called once per frame in `Update()` and processes only active touches. No allocations occur during recognition.
- **Battery polling**: Battery level is read from OS APIs. The mobile platform caches the value and does not poll the OS every frame; updates arrive via platform callbacks.
- **Quality switching**: Changing quality presets via `SetQualityPreset()` only updates the internal `MobileQualitySettings` struct. The rendering system must read these values and apply them, which may cause a brief stutter if shadow maps are reallocated.

---

## Troubleshooting

| Problem                              | Cause                                       | Solution                                          |
|--------------------------------------|---------------------------------------------|---------------------------------------------------|
| `Initialize()` returns `false`       | Platform support not compiled               | Ensure `ENABLE_MOBILE=ON` in CMake                |
| No gestures recognized               | `Update()` not called each frame            | Call `mobile.Update(deltaTime)` in the game loop  |
| Battery level always `-1`            | Running on desktop or simulator             | Expected behavior; battery API is hardware-dependent |
| Quality not changing on low battery  | `SetBatteryAwareScaling(false)` was called  | Ensure battery-aware scaling is enabled           |
| Touch coordinates seem wrong         | Not using normalized coordinates            | Touch x/y are [0, 1]; multiply by screen size if needed |
| Safe area insets are all zero        | Device has no notch or home indicator       | Expected; safe area is only nonzero on notched devices |
| Gesture callbacks not firing         | Callbacks registered after `Update()`       | Register callbacks before the first `Update()` call |

---

## Platform-Specific Notes

### iOS

- Touch events arrive via `UITouch` in the `UIView` responder chain. The platform layer converts these to `TouchEvent` structs and calls `ProcessTouchEvent()`.
- `GetBatteryLevel()` maps to `UIDevice.current.batteryLevel`.
- `GetSafeArea()` maps to `UIView.safeAreaInsets`.

### Android

- Touch events arrive via `MotionEvent` in the `Activity` or `SurfaceView`. The platform layer converts action types (`ACTION_DOWN`, `ACTION_MOVE`, `ACTION_UP`, `ACTION_CANCEL`) to `TouchEventType`.
- `GetBatteryLevel()` reads from `BatteryManager`.
- `GetSafeArea()` uses `WindowInsets` API (API level 28+) for display cutout insets.

---

## See Also

- [Input System](Input-System) — Keyboard/mouse input on desktop
- [Rendering and Graphics](Rendering-and-Graphics) — Quality settings and render scale
- [UI System](UI-System) — Touch-friendly UI layout
- [Build System and CMake Modules](Build-System-and-CMake-Modules) — Mobile build configuration
