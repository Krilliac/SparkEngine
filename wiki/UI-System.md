# UI System

SparkEngine provides a runtime UI framework for in-game HUD elements, menus, and overlays. Unlike the editor's Dear ImGui panels, this system is designed for shipped game UI with layout, anchoring, and input handling.

**Source:** `SparkEngine/Source/Engine/UI/UISystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `UISystem` | Top-level manager, owns the canvas |
| `UICanvas` | Root container (one per viewport), coordinate mapping |
| `UIPanel` | Container with layout, holds child widgets |
| `UILabel` | Text display |
| `UIButton` | Clickable button with hover/pressed states |
| `UIProgressBar` | Fill bar (health, loading, XP) |
| `UIImage` | Texture display (crosshairs, icons) |

## Widget Hierarchy

```
UICanvas (root, one per viewport)
  +-- UIPanel "HUD"
  |    +-- UILabel "Health: 100"
  |    +-- UIProgressBar (health bar)
  |    +-- UIImage (crosshair)
  +-- UIPanel "PauseMenu"
       +-- UILabel "PAUSED"
       +-- UIButton "Resume"
       +-- UIButton "Quit"
```

## Quick Start

```cpp
UISystem ui;
ui.Initialize(1920, 1080);

auto* hud = ui.GetCanvas().CreatePanel("HUD");
hud->SetAnchor(Anchor::TopLeft);

auto* healthLabel = hud->CreateLabel("health_text", "Health: 100");
healthLabel->SetPosition(20, 20);
healthLabel->SetFontSize(24);

auto* healthBar = hud->CreateProgressBar("health_bar");
healthBar->SetPosition(20, 50);
healthBar->SetSize(200, 20);
healthBar->SetValue(1.0f);

auto* btn = hud->CreateButton("resume", "Resume");
btn->OnClick([](){ ResumeGame(); });

// Per frame:
ui.Update(deltaTime);
ui.Render();
```

## Anchoring and Layout

Widgets can be anchored to screen positions and panels support automatic child layout:

```cpp
enum class Anchor {
    TopLeft, TopCenter, TopRight,
    MiddleLeft, Center, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
    Stretch  // Fill entire parent
};

enum class LayoutDirection {
    None,       // Manual positioning
    Horizontal, // Left-to-right
    Vertical    // Top-to-bottom
};

panel->SetLayout(LayoutDirection::Vertical);
panel->SetSpacing(10.0f);
panel->SetPadding(15.0f);
```

## Colors

```cpp
struct UIColor { float r, g, b, a; };

healthBar->SetFillColor(UIColor::Green());
healthBar->SetBackgroundColor({0.2f, 0.2f, 0.2f, 0.8f});
btn->SetNormalColor({0.3f, 0.3f, 0.3f, 0.9f});
btn->SetHoverColor({0.4f, 0.4f, 0.4f, 0.9f});
```

## Input Handling

The UI system consumes click events before they reach gameplay:

```cpp
if (ui.HandleClick(mouseX, mouseY)) {
    // Click was consumed by UI — do not process in gameplay
}
```

---

## See Also

- [Localization](Localization) — Localized text for UI labels
- [Loading System](Loading-System) — Loading screen progress bars
- [Input System](Input-System) — Mouse and keyboard input
- [SparkEditor](SparkEditor) — Editor UI (Dear ImGui, separate from runtime UI)
