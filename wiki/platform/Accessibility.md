# Accessibility

The Accessibility System provides engine-wide runtime accessibility features including colorblind simulation and correction (daltonization matrices for 5 modes), subtitle rendering with speaker tags, high-contrast UI, large text scaling, screen reader hooks, reduced motion, and one-handed input presets. It lives in the `Spark::Accessibility` namespace.

**Source:** `SparkEngine/Source/Engine/Accessibility/AccessibilitySystem.h`

## Overview

| Class / Struct | Responsibility |
|---|---|
| `AccessibilitySystem` | Singleton managing all accessibility features: settings, colorblind filters, subtitles, and feature flags |
| `AccessibilitySettings` | Struct holding all user-facing accessibility preferences |
| `SubtitleEntry` | A single subtitle to display on screen with speaker tag, duration, and styling |
| `ColorblindMode` | Enum selecting the active colorblind simulation/correction mode |
| `OneHandedMode` | Enum selecting one-handed input layout (none, left, right) |

## Key Enums and Types

### ColorblindMode

```cpp
enum class ColorblindMode : uint8_t
{
    None = 0,     // No correction applied
    Protanopia,   // Red-blind (L-cone deficiency)
    Deuteranopia, // Green-blind (M-cone deficiency)
    Tritanopia,   // Blue-blind (S-cone deficiency)
    Achromatopsia // Total color blindness (monochromacy)
};
```

### OneHandedMode

```cpp
enum class OneHandedMode : uint8_t
{
    None = 0,
    LeftHanded,
    RightHanded
};
```

### AccessibilitySettings

```cpp
struct AccessibilitySettings
{
    ColorblindMode colorblindMode = ColorblindMode::None;
    float colorblindStrength = 1.0f;        // Correction intensity (0 = off, 1 = full)
    bool highContrastUI = false;            // High-contrast editor/game UI
    bool largeText = false;                 // Force larger default font size
    float textScaleFactor = 1.0f;           // Global text scale multiplier
    bool subtitlesEnabled = false;          // Show subtitles for dialogue/audio
    float subtitleBackgroundOpacity = 0.7f; // Subtitle background alpha (0-1)
    bool screenReaderEnabled = false;       // Emit text descriptions for UI elements
    bool reducedMotion = false;             // Disable screen shake, reduce particles
    bool inputRemappingEnabled = false;     // Allow runtime key remapping
    OneHandedMode oneHandedMode = OneHandedMode::None;
};
```

### SubtitleEntry

```cpp
struct SubtitleEntry
{
    std::string text;            // The subtitle text content
    std::string speaker;         // Speaker name/tag (empty if narration)
    float duration = 3.0f;       // Display duration in seconds
    float fontSize = 18.0f;      // Font size in points
    uint32_t color = 0xFFFFFFFF; // RGBA packed color
    float remainingTime = 0.0f;  // Internal: time left before removal
};
```

## Quick Start

### Basic initialization

```cpp
#include "Engine/Accessibility/AccessibilitySystem.h"

auto& access = Spark::Accessibility::AccessibilitySystem::GetInstance();
access.Initialize();
```

### Applying colorblind correction

```cpp
using namespace Spark::Accessibility;

auto& access = AccessibilitySystem::GetInstance();
AccessibilitySettings s = access.GetSettings();
s.colorblindMode = ColorblindMode::Deuteranopia;
s.colorblindStrength = 0.8f; // 80% correction intensity
access.SetSettings(s);
```

### Applying the filter to a color

```cpp
float rgbIn[3] = {1.0f, 0.0f, 0.0f};  // Pure red
float rgbOut[3];
access.ApplyColorblindFilter(rgbOut, rgbIn);
// rgbOut now contains the daltonized color
```

### Enabling subtitles and pushing entries

```cpp
auto settings = access.GetSettings();
settings.subtitlesEnabled = true;
settings.textScaleFactor = 1.5f; // 150% text scale
access.SetSettings(settings);

Spark::Accessibility::SubtitleEntry sub;
sub.text = "Watch out for the enemy ahead!";
sub.speaker = "Squad Leader";
sub.duration = 4.0f;
sub.fontSize = 20.0f;
sub.color = 0xFFFF00FF; // Yellow, full alpha
access.PushSubtitle(sub);
```

### Per-frame update

```cpp
// In your main loop -- expires subtitles whose duration has elapsed
access.Update(dt);
```

### Reading active subtitles for rendering

```cpp
const auto& subtitles = access.GetActiveSubtitles();
for (const auto& sub : subtitles)
{
    RenderSubtitle(sub.text, sub.speaker, sub.fontSize,
                   sub.color, sub.remainingTime / sub.duration);
}
```

### Enabling high-contrast and reduced motion

```cpp
auto settings = access.GetSettings();
settings.highContrastUI = true;
settings.reducedMotion = true;
access.SetSettings(settings);

// In your VFX/camera system:
if (access.GetSettings().reducedMotion)
{
    // Skip screen shake, reduce particle counts, disable camera bob
    cameraShakeIntensity = 0.0f;
    particleSpawnRate *= 0.25f;
}
```

### One-handed input mode

```cpp
auto settings = access.GetSettings();
settings.oneHandedMode = Spark::Accessibility::OneHandedMode::LeftHanded;
settings.inputRemappingEnabled = true;
access.SetSettings(settings);

// The input system should query this setting to apply
// one-handed presets (e.g., remap right-hand keys to left side)
```

## Colorblind Daltonization Matrices

The system pre-builds 3x3 correction matrices for each `ColorblindMode` during `Initialize()`. These are row-major float arrays.

### Getting a correction matrix directly

```cpp
auto matrix = Spark::Accessibility::AccessibilitySystem::GetColorblindCorrectionMatrix(
    Spark::Accessibility::ColorblindMode::Protanopia
);
// matrix is std::array<float, 9> in row-major order
// matrix[row * 3 + col]
```

### Matrix values by mode

| Mode | Description | Matrix (row-major) |
|---|---|---|
| `None` | Identity | `1,0,0, 0,1,0, 0,0,1` |
| `Protanopia` | Red-blind | `0.567,0.433,0.000, 0.558,0.442,0.000, 0.000,0.242,0.758` |
| `Deuteranopia` | Green-blind | `0.625,0.375,0.000, 0.700,0.300,0.000, 0.000,0.300,0.700` |
| `Tritanopia` | Blue-blind | `0.950,0.050,0.000, 0.000,0.433,0.567, 0.000,0.475,0.525` |
| `Achromatopsia` | Monochromacy | `0.2126,0.7152,0.0722, 0.2126,0.7152,0.0722, 0.2126,0.7152,0.0722` |

The Achromatopsia matrix uses Rec. 709 luminance coefficients for perceptually accurate grayscale conversion.

### Shader integration

Pass the correction matrix as a uniform to your post-processing shader:

```cpp
auto mat = access.GetColorblindCorrectionMatrix(access.GetSettings().colorblindMode);
shader->SetUniformMatrix3f("u_colorblindMatrix", mat.data());
shader->SetUniform1f("u_colorblindStrength", access.GetSettings().colorblindStrength);
```

In the shader:

```hlsl
float3 corrected = mul(float3x3(u_colorblindMatrix), originalColor.rgb);
finalColor.rgb = lerp(originalColor.rgb, corrected, u_colorblindStrength);
```

## Feature Flags

The system includes a generic feature flag mechanism for extensibility:

```cpp
// Enable a custom accessibility feature
access.SetFeatureEnabled("screen_magnifier", true);
access.SetFeatureEnabled("dyslexia_font", true);

// Check if a feature is enabled
if (access.IsFeatureEnabled("screen_magnifier"))
{
    // Apply magnification to the UI
}

// Disable a feature
access.SetFeatureEnabled("screen_magnifier", false);
```

Feature flags are stored in an `std::unordered_map<std::string, bool>` and are cleared on `Shutdown()`.

## Configuration

### AccessibilitySettings Fields

| Field | Type | Default | Description |
|---|---|---|---|
| `colorblindMode` | `ColorblindMode` | `None` | Active colorblind correction mode |
| `colorblindStrength` | `float` | `1.0` | Correction intensity (0 = off, 1 = full) |
| `highContrastUI` | `bool` | `false` | Force high-contrast colors in UI |
| `largeText` | `bool` | `false` | Force larger default font size |
| `textScaleFactor` | `float` | `1.0` | Global text scale multiplier for all UI text |
| `subtitlesEnabled` | `bool` | `false` | Display subtitles for dialogue and audio |
| `subtitleBackgroundOpacity` | `float` | `0.7` | Background alpha for subtitle boxes (0-1) |
| `screenReaderEnabled` | `bool` | `false` | Emit text descriptions for UI elements |
| `reducedMotion` | `bool` | `false` | Disable screen shake, camera bob, reduce particles |
| `inputRemappingEnabled` | `bool` | `false` | Allow runtime key remapping |
| `oneHandedMode` | `OneHandedMode` | `None` | One-handed input preset selection |

### Subtitle Behavior

- `PushSubtitle()` only adds a subtitle if `subtitlesEnabled` is true in current settings
- The subtitle's `fontSize` is automatically multiplied by `textScaleFactor` when pushed
- `Update(dt)` decrements `remainingTime` each frame and removes expired entries
- Active subtitles are accessible via `GetActiveSubtitles()` for rendering

## Console Commands

| Method | Description |
|---|---|
| `Console_GetStatus()` | Returns initialization state, colorblind mode, subtitle status (with active count), high contrast, reduced motion, and text scale |

Example output:

```
AccessibilitySystem: initialized | Colorblind: Deuteranopia | Subtitles: on (active: 2) | HighContrast: off | ReducedMotion: on | TextScale: 1.500000
```

## Platform Compliance

The accessibility feature set is designed to support compliance with major platform and regulatory requirements:

| Standard | Relevant Features |
|---|---|
| **EU European Accessibility Act (EAA)** | Colorblind correction, subtitles, screen reader hooks, text scaling, high contrast |
| **Xbox Accessibility Guidelines (XAG)** | Colorblind modes, subtitle customization, reduced motion, input remapping |
| **PlayStation Certification** | Subtitle support, text scaling, colorblind options |
| **WCAG 2.1 (web-adjacent)** | High contrast, text scaling, reduced motion, keyboard navigation |

Use the settings struct to implement per-platform compliance checklists. The `screenReaderEnabled` flag is a hook point for platform-specific screen reader integration (e.g., Windows Narrator, VoiceOver on macOS).

## Integration

### With the Rendering Pipeline

Apply colorblind correction as a post-processing pass:

```cpp
void PostProcessPass::ApplyAccessibility()
{
    const auto& settings = AccessibilitySystem::GetInstance().GetSettings();
    if (settings.colorblindMode != ColorblindMode::None)
    {
        auto mat = AccessibilitySystem::GetColorblindCorrectionMatrix(settings.colorblindMode);
        m_colorblindShader->SetUniformMatrix3f("u_matrix", mat.data());
        m_colorblindShader->SetUniform1f("u_strength", settings.colorblindStrength);
        m_colorblindShader->Apply();
    }
}
```

### With the UI System

Query settings for text rendering:

```cpp
float GetEffectiveFontSize(float baseFontSize)
{
    const auto& settings = AccessibilitySystem::GetInstance().GetSettings();
    float size = baseFontSize * settings.textScaleFactor;
    if (settings.largeText)
        size = std::max(size, 24.0f);
    return size;
}

bool ShouldUseHighContrast()
{
    return AccessibilitySystem::GetInstance().GetSettings().highContrastUI;
}
```

### With the Dialogue System

Push subtitles from dialogue events:

```cpp
void OnDialogueLine(const std::string& speaker, const std::string& text, float duration)
{
    Spark::Accessibility::SubtitleEntry sub;
    sub.speaker = speaker;
    sub.text = text;
    sub.duration = duration;
    sub.fontSize = 18.0f;
    sub.color = GetSpeakerColor(speaker);

    AccessibilitySystem::GetInstance().PushSubtitle(sub);
}
```

### With PlatformInput

Apply one-handed mode presets:

```cpp
if (access.GetSettings().oneHandedMode == OneHandedMode::LeftHanded)
{
    // Remap WASD to arrow keys, right-hand actions to left-hand keys
    actionMap.RegisterAction("MoveForward", PlatformKeyCode::Up);
    actionMap.RegisterAction("Fire", PlatformKeyCode::Q);
}
```

### With the VFX / Camera Systems

Respect reduced motion:

```cpp
if (access.GetSettings().reducedMotion)
{
    screenShakeAmount = 0.0f;
    particleSpawnMultiplier = 0.25f;
    disableCameraBob = true;
    skipFlashEffects = true;
}
```

## API Reference

### AccessibilitySystem

| Method | Signature | Description |
|---|---|---|
| `GetInstance` | `static AccessibilitySystem& GetInstance()` | Get the singleton instance |
| `Initialize` | `void Initialize()` | Initialize system, build color matrices, reset settings |
| `Shutdown` | `void Shutdown()` | Clear subtitles, feature flags, reset state |
| `Update` | `void Update(float dt)` | Per-frame update; expires subtitles |
| `GetSettings` | `const AccessibilitySettings& GetSettings() const` | Get current settings (read-only) |
| `SetSettings` | `void SetSettings(const AccessibilitySettings& settings)` | Apply new settings |
| `ApplyColorblindFilter` | `void ApplyColorblindFilter(float* rgbOut, const float* rgbIn) const` | Apply daltonization to an RGB triplet |
| `GetColorblindCorrectionMatrix` | `static std::array<float, 9> GetColorblindCorrectionMatrix(ColorblindMode mode)` | Get the 3x3 correction matrix for a mode |
| `PushSubtitle` | `void PushSubtitle(const SubtitleEntry& entry)` | Push a subtitle for display (respects subtitlesEnabled) |
| `GetActiveSubtitles` | `const std::vector<SubtitleEntry>& GetActiveSubtitles() const` | Get all currently visible subtitles |
| `IsFeatureEnabled` | `bool IsFeatureEnabled(std::string_view feature) const` | Check if a named feature flag is enabled |
| `SetFeatureEnabled` | `void SetFeatureEnabled(std::string_view feature, bool enabled)` | Enable or disable a named feature flag |
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Human-readable status string |

## Thread Safety

`AccessibilitySystem` is **not thread-safe**. `SetSettings()`, `PushSubtitle()`, `Update()`, and `SetFeatureEnabled()` all modify internal state without synchronization. Call from the main thread only.

`GetColorblindCorrectionMatrix()` is a static method with no mutable state and is safe to call from any thread.

The cached color matrices (`m_colorMatrices`) are built once during `Initialize()` and only read thereafter, so they are effectively immutable during normal operation.

## See Also

- [Platform-Input](Platform-Input.md) -- Input remapping and one-handed mode integration
- [Telemetry-System](../advanced/Telemetry-System.md) -- Track accessibility feature usage
- [UI-System](../subsystems/UI-System.md) -- UI rendering with text scaling and high contrast
- [Dialogue-System](../subsystems/Dialogue-System.md) -- Subtitle integration with dialogue
