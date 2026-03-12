# UI System

SparkEngine provides a runtime UI framework for in-game HUD elements, menus, and overlays. Unlike the editor's Dear ImGui panels, this system is designed for shipped game UI with layout, anchoring, and input handling.

**Source:** `SparkEngine/Source/Engine/UI/UISystem.h`
**Namespace:** `Spark::UI`

## Overview

| Class | Responsibility |
|-------|---------------|
| `UISystem` | Top-level manager, owns the canvas, global visibility |
| `UICanvas` | Root container (one per viewport), coordinate mapping, panel management |
| `UIWidget` | Base class for all widgets (position, size, anchor, visibility, opacity) |
| `UIPanel` | Container with layout, holds child widgets, background color |
| `UILabel` | Text display with font size and color |
| `UIButton` | Clickable button with hover/pressed/normal color states |
| `UIProgressBar` | Fill bar (health, loading, XP) with fill and background colors |
| `UIImageWidget` | Texture display (crosshairs, icons) with tint color |

## Architecture

```
+-------------------------------------------------------------------+
|                          UISystem                                  |
|  m_canvas : UICanvas                                               |
|  m_visible : bool                                                  |
|                                                                    |
|  Initialize(width, height)                                         |
|  Update(deltaTime)  ──> m_canvas.Update(deltaTime)                 |
|  Render()           ──> m_canvas.Render()                          |
|  HandleClick(x, y)  ──> m_canvas.HandleClick(x, y)                |
|  OnResize(w, h)     ──> m_canvas.Resize(w, h)                     |
+-------------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------------+
|                          UICanvas                                  |
|  m_panels : vector<unique_ptr<UIPanel>>                            |
|  m_width, m_height : int                                           |
|                                                                    |
|  CreatePanel(name) ──> returns UIPanel*                            |
|  FindWidget(name)  ──> recursive search across all panels          |
|  RemovePanel(name)                                                 |
|  Update / Render / HandleClick  ──> delegates to each panel        |
+-------------------------------------------------------------------+
                              |
                              v
+-------------------------------------------------------------------+
|                          UIPanel (extends UIWidget)                 |
|  m_children : vector<unique_ptr<UIWidget>>                         |
|  m_layout : LayoutDirection (None / Horizontal / Vertical)         |
|  m_spacing, m_padding : float                                      |
|  m_bgColor : UIColor                                               |
|                                                                    |
|  CreateLabel / CreateButton / CreateProgressBar / CreateImage      |
|  CreatePanel (nested panels)                                       |
|  FindWidget(name) ──> recursive                                    |
|  RemoveWidget(name)                                                |
+-------------------------------------------------------------------+
                              |
                  +-----------+-----------+-----------+
                  |           |           |           |
              UILabel    UIButton   UIProgressBar  UIImageWidget
```

## Widget Hierarchy

```
UICanvas (root, one per viewport)
  +-- UIPanel "HUD"
  |    +-- UILabel "Health: 100"
  |    +-- UIProgressBar (health bar)
  |    +-- UIImageWidget (crosshair)
  +-- UIPanel "PauseMenu"
  |    +-- UILabel "PAUSED"
  |    +-- UIButton "Resume"
  |    +-- UIButton "Quit"
  +-- UIPanel "DialogueBox"
       +-- UILabel "speaker_name"
       +-- UILabel "dialogue_text"
       +-- UIPanel "ChoicesPanel"
            +-- UIButton "choice_0"
            +-- UIButton "choice_1"
            +-- UIButton "choice_2"
```

## Enums

### Anchor

```cpp
enum class Anchor
{
    TopLeft,      // Anchored to top-left corner
    TopCenter,    // Anchored to top-center
    TopRight,     // Anchored to top-right corner
    MiddleLeft,   // Anchored to middle-left
    Center,       // Anchored to screen center
    MiddleRight,  // Anchored to middle-right
    BottomLeft,   // Anchored to bottom-left
    BottomCenter, // Anchored to bottom-center
    BottomRight,  // Anchored to bottom-right
    Stretch       // Fill the entire parent area
};
```

### LayoutDirection

```cpp
enum class LayoutDirection
{
    None,       // Manual positioning (SetPosition per widget)
    Horizontal, // Left-to-right automatic arrangement
    Vertical    // Top-to-bottom automatic arrangement
};
```

## UIColor Struct

```cpp
struct UIColor
{
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

    static UIColor White();
    static UIColor Black();
    static UIColor Red();
    static UIColor Green();
    static UIColor Blue();
    static UIColor Yellow();
    static UIColor Transparent();
};
```

| Factory Method | RGBA Value |
|----------------|------------|
| `UIColor::White()` | `{1, 1, 1, 1}` |
| `UIColor::Black()` | `{0, 0, 0, 1}` |
| `UIColor::Red()` | `{1, 0, 0, 1}` |
| `UIColor::Green()` | `{0, 1, 0, 1}` |
| `UIColor::Blue()` | `{0, 0, 1, 1}` |
| `UIColor::Yellow()` | `{1, 1, 0, 1}` |
| `UIColor::Transparent()` | `{0, 0, 0, 0}` |

Custom colors use aggregate initialization: `UIColor{0.2f, 0.5f, 0.8f, 0.9f}`.

## API Reference

### UIWidget (Base Class)

All widgets inherit from `UIWidget`. These methods are available on every widget type.

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `UIWidget(const std::string& name)` | Create widget with a unique name |
| `GetName` | `const std::string& GetName() const` | Get the widget name |
| `SetPosition` | `void SetPosition(float x, float y)` | Set position relative to parent |
| `SetSize` | `void SetSize(float width, float height)` | Set widget dimensions |
| `SetAnchor` | `void SetAnchor(Anchor anchor)` | Set anchor point |
| `SetVisible` | `void SetVisible(bool visible)` | Show or hide the widget |
| `IsVisible` | `bool IsVisible() const` | Check visibility |
| `SetOpacity` | `void SetOpacity(float opacity)` | Set opacity (0.0-1.0) |
| `GetX` / `GetY` | `float GetX() const` | Get position components |
| `GetWidth` / `GetHeight` | `float GetWidth() const` | Get size components |
| `Update` | `virtual void Update(float deltaTime)` | Per-frame update (override) |
| `Render` | `virtual void Render() const` | Draw the widget (override) |
| `HandleClick` | `virtual bool HandleClick(float x, float y)` | Handle click; returns true if consumed |

Default member values: position `(0, 0)`, size `(100, 30)`, anchor `TopLeft`, visible `true`, opacity `1.0`.

### UILabel

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `UILabel(const std::string& name, const std::string& text)` | Create label with text |
| `SetText` | `void SetText(const std::string& text)` | Update displayed text |
| `GetText` | `const std::string& GetText() const` | Get current text |
| `SetFontSize` | `void SetFontSize(int size)` | Set font size (default: 16) |
| `SetColor` | `void SetColor(const UIColor& color)` | Set text color |

### UIButton

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `UIButton(const std::string& name, const std::string& label)` | Create button with label |
| `SetLabel` | `void SetLabel(const std::string& label)` | Update button text |
| `GetLabel` | `const std::string& GetLabel() const` | Get button text |
| `OnClick` | `void OnClick(std::function<void()> callback)` | Set click handler |
| `SetNormalColor` | `void SetNormalColor(const UIColor& color)` | Default state color |
| `SetHoverColor` | `void SetHoverColor(const UIColor& color)` | Mouse-over color |
| `SetPressedColor` | `void SetPressedColor(const UIColor& color)` | Click-down color |

Default colors: normal `{0.3, 0.3, 0.3, 0.9}`, hover `{0.4, 0.4, 0.4, 0.9}`, pressed `{0.2, 0.2, 0.2, 0.9}`.

### UIProgressBar

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `UIProgressBar(const std::string& name)` | Create progress bar |
| `SetValue` | `void SetValue(float value)` | Set fill (clamped 0.0-1.0) |
| `GetValue` | `float GetValue() const` | Get current fill level |
| `SetFillColor` | `void SetFillColor(const UIColor& color)` | Set fill portion color |
| `SetBackgroundColor` | `void SetBackgroundColor(const UIColor& color)` | Set empty portion color |

Default: value `1.0`, fill color `Green()`, background `{0.2, 0.2, 0.2, 0.8}`.

### UIImageWidget

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `UIImageWidget(const std::string& name, const std::string& texturePath = "")` | Create image widget |
| `SetTexturePath` | `void SetTexturePath(const std::string& path)` | Set texture file path |
| `GetTexturePath` | `const std::string& GetTexturePath() const` | Get texture path |
| `SetTint` | `void SetTint(const UIColor& color)` | Set color tint |

### UIPanel

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `UIPanel(const std::string& name)` | Create panel container |
| `SetLayout` | `void SetLayout(LayoutDirection layout)` | Set child layout mode |
| `SetSpacing` | `void SetSpacing(float spacing)` | Space between children (default: 5) |
| `SetPadding` | `void SetPadding(float padding)` | Internal padding (default: 10) |
| `SetBackgroundColor` | `void SetBackgroundColor(const UIColor& color)` | Panel background |
| `CreateLabel` | `UILabel* CreateLabel(const std::string& name, const std::string& text)` | Add a label child |
| `CreateButton` | `UIButton* CreateButton(const std::string& name, const std::string& label)` | Add a button child |
| `CreateProgressBar` | `UIProgressBar* CreateProgressBar(const std::string& name)` | Add a progress bar child |
| `CreateImage` | `UIImageWidget* CreateImage(const std::string& name, const std::string& texturePath = "")` | Add an image child |
| `CreatePanel` | `UIPanel* CreatePanel(const std::string& name)` | Add a nested panel child |
| `FindWidget` | `UIWidget* FindWidget(const std::string& name)` | Recursive search by name |
| `RemoveWidget` | `void RemoveWidget(const std::string& name)` | Remove child by name |

### UICanvas

| Method | Signature | Description |
|--------|-----------|-------------|
| `Initialize` | `void Initialize(int width, int height)` | Set screen resolution |
| `CreatePanel` | `UIPanel* CreatePanel(const std::string& name)` | Create top-level panel |
| `FindWidget` | `UIWidget* FindWidget(const std::string& name)` | Search all panels |
| `RemovePanel` | `void RemovePanel(const std::string& name)` | Remove a panel by name |
| `Update` | `void Update(float deltaTime)` | Update all panels |
| `Render` | `void Render() const` | Render all visible panels |
| `HandleClick` | `bool HandleClick(float x, float y)` | Process click event |
| `GetWidth` / `GetHeight` | `int GetWidth() const` | Get canvas dimensions |
| `Resize` | `void Resize(int width, int height)` | Handle resolution change |

### UISystem

| Method | Signature | Description |
|--------|-----------|-------------|
| `Initialize` | `void Initialize(int screenWidth, int screenHeight)` | Initialize the UI system |
| `GetCanvas` | `UICanvas& GetCanvas()` | Get the root canvas (mutable) |
| `GetCanvas` | `const UICanvas& GetCanvas() const` | Get the root canvas (const) |
| `Update` | `void Update(float deltaTime)` | Update all UI elements |
| `Render` | `void Render()` | Render all UI elements |
| `OnResize` | `void OnResize(int width, int height)` | Handle window resize |
| `HandleClick` | `bool HandleClick(float x, float y)` | Process click; returns true if consumed |
| `SetVisible` | `void SetVisible(bool visible)` | Show/hide all UI |
| `IsVisible` | `bool IsVisible() const` | Check global visibility |
| `Console_GetStatus` | `std::string Console_GetStatus() const` | Console status string |

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

Widgets can be anchored to screen positions, and panels support automatic child layout:

```cpp
panel->SetLayout(LayoutDirection::Vertical);
panel->SetSpacing(10.0f);
panel->SetPadding(15.0f);
```

### Layout Behavior

When a panel uses `LayoutDirection::Vertical`, children are arranged top-to-bottom starting at `(padding, padding)`, with `spacing` pixels between each child. The children's `SetPosition` calls are ignored in auto-layout mode. Similarly, `LayoutDirection::Horizontal` arranges children left-to-right.

When layout is `None`, children use their manually set positions relative to the panel's top-left corner plus padding.

### Anchor Coordinate Reference

```
+------TopLeft----TopCenter----TopRight------+
|                                             |
MiddleLeft        Center        MiddleRight   |
|                                             |
+---BottomLeft--BottomCenter--BottomRight-----+
```

The `Stretch` anchor causes the widget to fill its parent's entire area, ignoring position and size settings.

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
if (input.WasKeyPressed(VK_ESCAPE))
{
    bool paused = !pausePanel->IsVisible();
    pausePanel->SetVisible(paused);
    SetGamePaused(paused);
}
```

## Input Handling

The UI system consumes click events before they reach gameplay. Call `HandleClick` before processing gameplay input:

```cpp
// In your input processing loop:
if (ui.HandleClick(mouseX, mouseY))
{
    // Click was consumed by a UI button -- do not process in gameplay
    return;
}

// Otherwise, process as gameplay input (e.g., fire weapon)
```

The click propagates through the canvas in reverse panel order (last-created panels are checked first, acting as a z-order). Within a panel, children are checked in reverse order. The first widget whose bounding box contains the click coordinates and whose `HandleClick` returns `true` consumes the event.

## Resizing

```cpp
// Handle window resize events
ui.OnResize(newWidth, newHeight);
```

This delegates to `UICanvas::Resize()`, which updates the canvas dimensions. Anchored widgets are repositioned based on their anchor points and the new resolution.

## Internal Implementation

### Ownership Model

- `UISystem` owns a single `UICanvas` by value.
- `UICanvas` owns top-level `UIPanel` instances via `std::vector<std::unique_ptr<UIPanel>>`.
- Each `UIPanel` owns its children via `std::vector<std::unique_ptr<UIWidget>>`.
- Pointers returned by `Create*` methods are non-owning. The canvas/panel retains ownership.
- Removing a widget (`RemoveWidget` / `RemovePanel`) destroys the widget and invalidates any raw pointers to it.

### Render Order

Panels are rendered in creation order (first-created panel renders first, appearing behind later panels). Within a panel, children render in creation order. This means the last-created widget appears on top.

### Update Cycle

Each frame follows this sequence:

1. `UISystem::Update(deltaTime)` calls `UICanvas::Update(deltaTime)`.
2. `UICanvas::Update` iterates all panels, calling `UIPanel::Update(deltaTime)`.
3. Each panel applies layout calculations (if layout is not `None`), then calls `Update(deltaTime)` on each visible child.
4. `UISystem::Render()` calls `UICanvas::Render()`.
5. Each visible panel renders its background, then renders each visible child.

## Error Handling

| Scenario | Behavior |
|----------|----------|
| `CreatePanel` with duplicate name | Creates a new panel; previous panel remains (names are not unique keys) |
| `FindWidget` with unknown name | Returns `nullptr` |
| `RemoveWidget` with unknown name | No effect (silent) |
| `HandleClick` outside all widgets | Returns `false` |
| `SetValue` on progress bar outside 0-1 | Clamped to [0.0, 1.0] via `std::clamp` |
| `OnClick` with null callback | Safe; button click does nothing |
| `Render` before `Initialize` | Renders nothing (canvas has default 1920x1080 dimensions) |

## Performance

- The widget tree is walked linearly each frame for both `Update` and `Render`. With typical game UIs (under 100 widgets), this is negligible.
- `FindWidget` performs a depth-first recursive search. For frequent lookups, cache the returned pointer instead of calling `FindWidget` every frame.
- Hidden widgets (`SetVisible(false)`) are skipped during both `Update` and `Render`, so hiding panels (e.g., pause menu) has zero per-frame cost.
- Text rendering is the most expensive widget operation. Avoid changing `SetText` every frame unless the text actually changed.

## Thread Safety

The UI system is **not thread-safe**. All UI operations must occur on the main thread, including:
- Creating, removing, and modifying widgets
- Calling `Update`, `Render`, and `HandleClick`
- Registering click callbacks

Button `OnClick` callbacks execute synchronously on the main thread during `HandleClick`.

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| Widget not visible | Panel or widget `SetVisible(false)` | Check visibility of entire parent chain |
| Button click not registering | UI `HandleClick` not called before gameplay input | Call `ui.HandleClick()` first in input loop |
| Widget in wrong position | Wrong anchor or parent panel | Verify anchor; check if panel uses auto-layout |
| Progress bar appears empty | `SetValue(0.0f)` or value not updated | Ensure per-frame value update |
| Text overlapping | Manual positions conflict | Use auto-layout (`SetLayout(Vertical)`) |
| Widgets not resizing with window | `OnResize` not called | Wire window resize events to `ui.OnResize()` |
| Click goes through UI to gameplay | `HandleClick` return value not checked | Only process gameplay input if `HandleClick` returns `false` |

---

## See Also

- [Localization](Localization) -- Localized text for UI labels
- [Loading System](Loading-System) -- Loading screen progress bars
- [Input System](Input-System) -- Mouse and keyboard input
- [Dialogue System](Dialogue-System) -- Dialogue UI integration
- [SparkEditor](SparkEditor) -- Editor UI (Dear ImGui, separate from runtime UI)
