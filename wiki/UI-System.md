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

## Building a HUD

A complete FPS HUD example:

```cpp
UISystem ui;
ui.Initialize(1920, 1080);
auto& canvas = ui.GetCanvas();

// Health panel (bottom-left)
auto* healthPanel = canvas.CreatePanel("HealthPanel");
healthPanel->SetAnchor(Anchor::BottomLeft);
healthPanel->SetLayout(LayoutDirection::Vertical);
healthPanel->SetSpacing(5.0f);
healthPanel->SetPadding(20.0f);
healthPanel->SetBackgroundColor({0.0f, 0.0f, 0.0f, 0.5f});

auto* healthLabel = healthPanel->CreateLabel("hp_label", "Health");
healthLabel->SetFontSize(16);
healthLabel->SetColor(UIColor::White());

auto* healthBar = healthPanel->CreateProgressBar("hp_bar");
healthBar->SetSize(200, 20);
healthBar->SetValue(1.0f);
healthBar->SetFillColor(UIColor::Green());
healthBar->SetBackgroundColor({0.2f, 0.2f, 0.2f, 0.8f});

// Crosshair (center)
auto* crosshair = canvas.CreatePanel("CrosshairPanel");
crosshair->SetAnchor(Anchor::Center);
auto* crosshairImg = crosshair->CreateImage("crosshair_img");
crosshairImg->SetTexturePath("Assets/UI/crosshair.png");
crosshairImg->SetSize(32, 32);
crosshairImg->SetTint(UIColor::White());

// Ammo counter (bottom-right)
auto* ammoPanel = canvas.CreatePanel("AmmoPanel");
ammoPanel->SetAnchor(Anchor::BottomRight);
auto* ammoLabel = ammoPanel->CreateLabel("ammo_text", "30 / 120");
ammoLabel->SetFontSize(28);
ammoLabel->SetColor(UIColor::White());
ammoLabel->SetPosition(20, 20);

// Update health bar each frame
healthBar->SetValue(player.health / player.maxHealth);
ammoLabel->SetText(std::to_string(weapon.ammo) + " / " + std::to_string(weapon.reserveAmmo));
```

## Pause Menu Example

```cpp
auto* pausePanel = canvas.CreatePanel("PauseMenu");
pausePanel->SetAnchor(Anchor::Center);
pausePanel->SetLayout(LayoutDirection::Vertical);
pausePanel->SetSpacing(15.0f);
pausePanel->SetPadding(30.0f);
pausePanel->SetBackgroundColor({0.1f, 0.1f, 0.1f, 0.9f});
pausePanel->SetVisible(false);  // Hidden by default

auto* title = pausePanel->CreateLabel("pause_title", "PAUSED");
title->SetFontSize(36);
title->SetColor(UIColor::White());

auto* resumeBtn = pausePanel->CreateButton("resume_btn", "Resume");
resumeBtn->SetNormalColor({0.3f, 0.3f, 0.3f, 0.9f});
resumeBtn->SetHoverColor({0.4f, 0.5f, 0.4f, 0.9f});
resumeBtn->SetPressedColor({0.2f, 0.4f, 0.2f, 0.9f});
resumeBtn->OnClick([&]() {
    pausePanel->SetVisible(false);
    SetGamePaused(false);
});

auto* quitBtn = pausePanel->CreateButton("quit_btn", "Quit to Menu");
quitBtn->SetNormalColor({0.3f, 0.3f, 0.3f, 0.9f});
quitBtn->SetHoverColor({0.5f, 0.3f, 0.3f, 0.9f});
quitBtn->OnClick([&]() { LoadScene("MainMenu"); });

// Toggle pause with Escape
if (input.WasKeyPressed(VK_ESCAPE)) {
    bool paused = !pausePanel->IsVisible();
    pausePanel->SetVisible(paused);
    SetGamePaused(paused);
}
```

## Input Handling

The UI system consumes click events before they reach gameplay:

```cpp
if (ui.HandleClick(mouseX, mouseY)) {
    // Click was consumed by UI — do not process in gameplay
}
```

## Resizing

```cpp
// Handle window resize events
ui.OnResize(newWidth, newHeight);
```

---

## See Also

- [Localization](Localization) — Localized text for UI labels
- [Loading System](Loading-System) — Loading screen progress bars
- [Input System](Input-System) — Mouse and keyboard input
- [SparkEditor](SparkEditor) — Editor UI (Dear ImGui, separate from runtime UI)
