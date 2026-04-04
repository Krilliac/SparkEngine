# UI Layout Extensions

The UI Layout Extensions module provides declarative layout containers, interactive input widgets, and data binding for the SparkEngine UI system. It extends the base `UIWidget`/`UIPanel` classes from `UISystem.h` with flex-box layouts, grid layouts, scrollable viewports, text inputs, sliders, dropdowns, and a JSON-based layout loader.

**Source:** `SparkEngine/Source/Engine/UI/UILayoutExtensions.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `UIFlexContainer` | Flex-box style container that distributes children along horizontal or vertical axis |
| `UIGridLayout` | Grid-based layout placing children into rows and columns with uniform cell sizes |
| `UIScrollView` | Scrollable viewport over content larger than the visible area |
| `UITextInput` | Single-line text input widget with cursor, selection, and placeholder |
| `UISlider` | Numeric slider with configurable range, step, and orientation |
| `UIDropdown` | Dropdown selection widget with labeled options |
| `UIDataBinding<T>` | Templated one-way data binding from a source pointer to formatted display text |
| `UILayoutLoader` | Static utility that parses JSON layout descriptions into a UIPanel hierarchy |

All classes live in the `Spark::UI` namespace and inherit from `UIWidget` (defined in `UISystem.h`).

## Key Enums and Types

### FlexAlign

Controls how children are distributed within a `UIFlexContainer`:

```cpp
enum class FlexAlign : uint8_t
{
    Start,        // Pack children toward the start edge
    Center,       // Center children along the axis
    End,          // Pack children toward the end edge
    Stretch,      // Stretch children to fill the axis
    SpaceBetween, // Equal space between children, none at edges
    SpaceAround   // Equal space around each child
};
```

### Padding

Four-sided padding values used by `UIFlexContainer`:

```cpp
struct Padding
{
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float left = 0.0f;
};
```

### LayoutDirection

Inherited from `UISystem.h`, used by both `UIFlexContainer` and `UISlider`:

```cpp
enum class LayoutDirection
{
    Horizontal,
    Vertical
};
```

## Quick Start

### Building a Toolbar with UIFlexContainer

```cpp
using namespace Spark::UI;

// Create a horizontal toolbar container
UIFlexContainer toolbar("main_toolbar");
toolbar.SetPosition(0.0f, 0.0f);
toolbar.SetSize(800.0f, 48.0f);
toolbar.SetDirection(LayoutDirection::Horizontal);
toolbar.SetAlignment(FlexAlign::Center);
toolbar.SetSpacing(8.0f);
toolbar.SetPadding(4.0f, 8.0f, 4.0f, 8.0f);

// Add buttons (non-owning pointers, owned by the parent panel)
toolbar.AddChild(&saveButton);
toolbar.AddChild(&loadButton);
toolbar.AddChild(&undoButton);
toolbar.AddChild(&redoButton);

// Compute child positions
toolbar.CalculateLayout();
```

### Creating a Settings Grid with UIGridLayout

```cpp
UIGridLayout settingsGrid("settings_grid");
settingsGrid.SetPosition(20.0f, 100.0f);
settingsGrid.SetSize(400.0f, 300.0f);
settingsGrid.SetColumns(2);
settingsGrid.SetRows(4);
settingsGrid.SetCellSpacing(8.0f);

// Place label-widget pairs into the grid
settingsGrid.SetCell(0, 0, &volumeLabel);
settingsGrid.SetCell(0, 1, &volumeSlider);
settingsGrid.SetCell(1, 0, &brightnessLabel);
settingsGrid.SetCell(1, 1, &brightnessSlider);
settingsGrid.SetCell(2, 0, &resolutionLabel);
settingsGrid.SetCell(2, 1, &resolutionDropdown);
settingsGrid.SetCell(3, 0, &fullscreenLabel);
settingsGrid.SetCell(3, 1, &fullscreenToggle);

settingsGrid.CalculateLayout();
```

### Scrollable Content with UIScrollView

```cpp
UIScrollView scrollView("log_scroll");
scrollView.SetPosition(10.0f, 10.0f);
scrollView.SetViewportSize(300.0f, 200.0f);
scrollView.SetContentSize(300.0f, 1500.0f);  // Content taller than viewport

// Scroll to a specific position
scrollView.SetScrollOffset(0.0f, 100.0f);

// Scroll by delta (e.g., from mouse wheel)
scrollView.ScrollBy(0.0f, -30.0f);

// Query scroll state
auto [offsetX, offsetY] = scrollView.GetScrollOffset();
auto [maxX, maxY] = scrollView.GetMaxScroll();
bool canScroll = scrollView.IsScrollable();

// Get the visible content rectangle
UIScrollView::Rect visible = scrollView.GetVisibleRect();
```

### Interactive Text Input

```cpp
UITextInput nameInput("player_name");
nameInput.SetPosition(100.0f, 50.0f);
nameInput.SetSize(200.0f, 30.0f);
nameInput.SetPlaceholder("Enter your name...");
nameInput.SetMaxLength(32);

// Listen for changes
nameInput.OnTextChanged([](const std::string& text) {
    std::println("Name changed to: {}", text);
});

// Programmatic control
nameInput.SetText("Player1");
nameInput.SetCursorPosition(7);
nameInput.SelectAll();
nameInput.SetFocused(true);

// Read-only mode for display
nameInput.SetReadOnly(true);
```

### Numeric Slider

```cpp
UISlider volumeSlider("master_volume");
volumeSlider.SetPosition(100.0f, 80.0f);
volumeSlider.SetSize(200.0f, 24.0f);
volumeSlider.SetRange(0.0f, 1.0f);
volumeSlider.SetStep(0.05f);  // Snap to 5% increments
volumeSlider.SetValue(0.75f);
volumeSlider.SetOrientation(LayoutDirection::Horizontal);

volumeSlider.OnValueChanged([](float value) {
    AudioEngine::SetMasterVolume(value);
});
```

### Dropdown Selection

```cpp
UIDropdown resolutionDropdown("resolution");
resolutionDropdown.SetPosition(100.0f, 120.0f);
resolutionDropdown.SetSize(200.0f, 30.0f);
resolutionDropdown.SetPlaceholder("Select resolution...");

resolutionDropdown.AddOption("1280x720", "720p");
resolutionDropdown.AddOption("1920x1080", "1080p");
resolutionDropdown.AddOption("2560x1440", "1440p");
resolutionDropdown.AddOption("3840x2160", "4K");

resolutionDropdown.SetSelectedIndex(1);  // Default to 1080p

resolutionDropdown.OnSelectionChanged([](uint32_t index, const std::string& value) {
    std::println("Resolution set to: {} (index {})", value, index);
    ApplyResolution(value);
});

// Query state
std::string selected = resolutionDropdown.GetSelectedValue();  // "1080p"
bool isOpen = resolutionDropdown.IsOpen();
resolutionDropdown.Toggle();  // Open/close the dropdown list
```

### Data Binding

```cpp
// Bind a health value to formatted display text
float playerHealth = 85.0f;

UIDataBinding<float> healthBinding;
healthBinding.Bind(&playerHealth);
healthBinding.SetFormatter([](const float& hp) {
    return std::format("HP: {:.0f}", hp);
});

// Each frame, call Update() to read the source and format
healthBinding.Update();
std::string display = healthBinding.GetFormattedText();  // "HP: 85"
float raw = healthBinding.GetBoundValue();               // 85.0f
```

### Loading Layouts from JSON

```cpp
std::string_view layoutJson = R"({
    "children": [
        { "type": "label", "name": "title", "text": "Settings", "x": 10, "y": 10 },
        { "type": "button", "name": "apply", "text": "Apply", "x": 10, "y": 50, "w": 100, "h": 30 },
        { "type": "progressbar", "name": "loading", "x": 10, "y": 90, "w": 200, "h": 20 },
        { "type": "image", "name": "logo", "src": "logo.png", "x": 10, "y": 120, "w": 64, "h": 64 },
        {
            "type": "panel", "name": "subpanel",
            "children": [
                { "type": "label", "name": "sub_title", "text": "Sub Panel", "x": 5, "y": 5 }
            ]
        }
    ]
})";

UIPanel* parent = /* your root panel */;
bool success = UILayoutLoader::LoadFromJSON(layoutJson, parent);
```

## Configuration

### JSON Layout Format

The `UILayoutLoader` supports a minimal JSON format for declarative UI layouts. Each widget node has:

| Property | Type | Description |
|----------|------|-------------|
| `type` | string | Widget type: `"label"`, `"button"`, `"progressbar"`, `"image"`, `"panel"` |
| `name` | string | Unique widget name (required) |
| `text` | string | Display text for labels and buttons |
| `src` | string | Image source path (for `"image"` type) |
| `x` | float | X position |
| `y` | float | Y position |
| `w` | float | Width |
| `h` | float | Height |
| `children` | array | Nested widget definitions (for `"panel"` type) |

Panels support recursive nesting via the `children` array. The loader processes each `{ ... }` block inside the `"children"` array and creates widgets via the parent panel's factory methods (`CreateLabel`, `CreateButton`, `CreateProgressBar`, `CreateImage`, `CreatePanel`).

### Flex Layout Modes

| FlexAlign | Behavior |
|-----------|----------|
| `Start` | Children packed at the beginning of the axis |
| `Center` | Children centered, equal free space on both sides |
| `End` | Children packed at the end of the axis |
| `Stretch` | Children stretched to fill the cross-axis |
| `SpaceBetween` | Free space divided equally between children only |
| `SpaceAround` | Free space divided equally around each child |

## Integration

### With UISystem

All layout extension classes inherit from `UIWidget` (defined in `UISystem.h`). They work within the existing `UIPanel` hierarchy:

```cpp
// Flex containers and grids are widgets, so they can be nested
UIFlexContainer mainLayout("main");
mainLayout.AddChild(&toolbar);    // toolbar is another UIFlexContainer
mainLayout.AddChild(&content);    // content is a UIGridLayout
mainLayout.CalculateLayout();
```

### With the ECS

Data bindings connect ECS component values to UI display:

```cpp
auto& transform = registry.get<TransformComponent>(entity);
UIDataBinding<float> xBinding;
xBinding.Bind(&transform.position.x);
xBinding.SetFormatter([](const float& x) {
    return std::format("X: {:.2f}", x);
});
```

### With the Event System

Input widgets fire callbacks that can integrate with the engine's event bus:

```cpp
slider.OnValueChanged([&eventBus](float value) {
    eventBus.Publish(VolumeChangedEvent{value});
});
```

## API Reference

### UIFlexContainer

| Method | Description |
|--------|-------------|
| `UIFlexContainer(const string& name)` | Construct with widget name |
| `SetDirection(LayoutDirection)` | Set horizontal or vertical layout |
| `GetDirection() -> LayoutDirection` | Get current direction |
| `SetAlignment(FlexAlign)` | Set child distribution mode |
| `GetAlignment() -> FlexAlign` | Get current alignment |
| `SetSpacing(float)` | Set gap between children in pixels |
| `SetPadding(float, float, float, float)` | Set top, right, bottom, left padding |
| `AddChild(UIWidget*)` | Add a child widget (non-owning) |
| `CalculateLayout()` | Distribute children according to settings |

### UIGridLayout

| Method | Description |
|--------|-------------|
| `UIGridLayout(const string& name)` | Construct with widget name |
| `SetColumns(uint32_t)` | Set number of columns (min 1) |
| `SetRows(uint32_t)` | Set number of rows (min 1) |
| `SetCellSpacing(float)` | Set spacing between cells in pixels |
| `SetCell(uint32_t row, uint32_t col, UIWidget*)` | Place a widget in a specific cell |
| `CalculateLayout()` | Compute uniform cell positions and sizes |

### UIScrollView

| Method | Description |
|--------|-------------|
| `UIScrollView(const string& name)` | Construct with widget name |
| `SetContentSize(float w, float h)` | Set total scrollable content size |
| `SetViewportSize(float w, float h)` | Set visible viewport size |
| `SetScrollOffset(float x, float y)` | Set absolute scroll offset (clamped) |
| `GetScrollOffset() -> pair<float, float>` | Get current (offsetX, offsetY) |
| `GetMaxScroll() -> pair<float, float>` | Get maximum (maxX, maxY) |
| `ScrollBy(float dx, float dy)` | Scroll by delta, clamped to valid range |
| `IsScrollable() -> bool` | True if content exceeds viewport |
| `GetVisibleRect() -> Rect` | Visible rectangle in content coordinates |

### UITextInput

| Method | Description |
|--------|-------------|
| `UITextInput(const string& name)` | Construct with widget name |
| `SetText(const string&)` | Set text content (respects maxLength and readOnly) |
| `GetText() -> const string&` | Get current text |
| `SetPlaceholder(const string&)` | Set placeholder text |
| `SetMaxLength(uint32_t)` | Set max character count (0 = unlimited) |
| `SetReadOnly(bool)` | Enable/disable read-only mode |
| `OnTextChanged(function<void(const string&)>)` | Register text change callback |
| `SetCursorPosition(uint32_t)` | Set cursor position (clamped) |
| `GetCursorPosition() -> uint32_t` | Get cursor position |
| `SelectAll()` | Select all text |
| `IsFocused() -> bool` | Check focus state |
| `SetFocused(bool)` | Set focus state |

### UISlider

| Method | Description |
|--------|-------------|
| `UISlider(const string& name)` | Construct with widget name |
| `SetRange(float min, float max)` | Set valid range |
| `SetValue(float)` | Set value (clamped and snapped to step) |
| `GetValue() -> float` | Get current value |
| `SetStep(float)` | Set step increment (0 = continuous) |
| `OnValueChanged(function<void(float)>)` | Register value change callback |
| `SetOrientation(LayoutDirection)` | Set horizontal or vertical |

### UIDropdown

| Method | Description |
|--------|-------------|
| `UIDropdown(const string& name)` | Construct with widget name |
| `AddOption(const string& label, const string& value)` | Add a selectable option |
| `RemoveOption(const string& value)` | Remove option by value |
| `SetSelectedIndex(uint32_t)` | Set selected option by index |
| `GetSelectedIndex() -> uint32_t` | Get selected index |
| `GetSelectedValue() -> string` | Get value string of selected option |
| `OnSelectionChanged(function<void(uint32_t, const string&)>)` | Register selection callback |
| `SetPlaceholder(const string&)` | Set placeholder text |
| `IsOpen() -> bool` | Check if dropdown list is open |
| `Toggle()` | Toggle open/closed state |

### UIDataBinding\<T\>

| Method | Description |
|--------|-------------|
| `Bind(T* source)` | Bind to a data source (non-owning) |
| `SetFormatter(function<string(const T&)>)` | Set value-to-string formatter |
| `Update()` | Read source and cache formatted string |
| `GetBoundValue() -> const T&` | Get last-read value |
| `GetFormattedText() -> const string&` | Get formatted display text |

### UILayoutLoader

| Method | Description |
|--------|-------------|
| `LoadFromJSON(string_view json, UIPanel* parent) -> bool` | Parse JSON and create widgets under parent |

## Thread Safety

All UI layout extension classes are **not thread-safe**. They are designed to be created, configured, and updated on the **main thread** alongside the rest of the UI system. Data bindings read source pointers on `Update()`, so the bound data must not be modified concurrently from another thread without external synchronization.

## See Also

- [[UI-System]] -- Base UIWidget, UIPanel, and UIButton classes
- [[UI-Rendering]] -- How UI widgets are rendered on screen
- [[ECS-Overview]] -- Entity Component System for data binding sources
- [[Event-System]] -- Event bus for widget callback integration
