# Editor Tutorials

The Editor Tutorial System provides interactive, step-by-step guided tutorials for the SparkEditor. Tutorials are sequences of steps that highlight panels, show tooltips, display messages, and wait for user actions. The system ships with built-in tutorials covering common editor workflows and supports registering custom tutorials for project-specific guidance.

**Source:** `SparkEditor/Source/Core/TutorialSystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `TutorialSystem` | Singleton that manages tutorial registration, playback, step advancement, and completion tracking |
| `TutorialSequence` | A named, ordered sequence of tutorial steps with difficulty classification |
| `TutorialStep` | A single instruction within a tutorial (highlight, tooltip, message, or action wait) |
| `TutorialStepType` | Enum defining what kind of action a step performs |
| `TooltipPosition` | Enum for tooltip placement relative to a target panel |
| `TutorialDifficulty` | Enum classifying tutorial difficulty level |

All types live in the `SparkEditor` namespace.

## Key Enums and Types

### TutorialStepType

Defines the kind of action each tutorial step performs:

```cpp
enum class TutorialStepType : uint8_t
{
    HighlightPanel, // Highlight a named editor panel
    ShowTooltip,    // Show a tooltip near a target panel
    WaitForAction,  // Block until the user performs a specific action
    ShowMessage,    // Display a message dialog
    WaitForInput    // Block until any input is received
};
```

### TooltipPosition

Controls where tooltips appear relative to their target panel:

```cpp
enum class TooltipPosition : uint8_t
{
    Top,
    Bottom,
    Left,
    Right
};
```

### TutorialDifficulty

Classifies tutorials for filtering and display:

```cpp
enum class TutorialDifficulty : uint8_t
{
    Beginner,
    Intermediate,
    Advanced
};
```

### TutorialStep

A single step within a tutorial sequence:

```cpp
struct TutorialStep
{
    TutorialStepType type = TutorialStepType::ShowMessage;     // What this step does
    std::string targetPanel;                                   // Panel name for highlight/tooltip
    std::string message;                                       // Instruction text shown to the user
    TooltipPosition tooltipPosition = TooltipPosition::Bottom; // Tooltip placement
    std::string requiredAction;   // Action string for WaitForAction steps
    float autoAdvanceDelay = 0.f; // Seconds before auto-advancing (0 = manual)
};
```

### TutorialSequence

A complete tutorial definition:

```cpp
struct TutorialSequence
{
    std::string name;                                             // Unique tutorial name
    std::string description;                                      // Short description for the tutorial list
    std::vector<TutorialStep> steps;                              // Ordered steps
    TutorialDifficulty difficulty = TutorialDifficulty::Beginner; // Difficulty tag
};
```

## Quick Start

### Starting a Built-in Tutorial

The system registers three built-in tutorials on `Initialize()`:

```cpp
auto& tut = SparkEditor::TutorialSystem::GetInstance();
tut.Initialize();

// Start the introductory tutorial
tut.StartTutorial("Getting Started");
```

### Per-Frame Update

Call `Update()` each frame to handle auto-advance timers:

```cpp
// In your main editor loop
void EditorApplication::OnUpdate(float deltaTime)
{
    auto& tut = SparkEditor::TutorialSystem::GetInstance();
    tut.Update(deltaTime);

    // Render the current step overlay
    if (const auto* step = tut.GetCurrentStep())
    {
        RenderTutorialOverlay(*step);
    }
}
```

### Rendering Tutorial Overlays

Use the current step's data to render appropriate UI overlays:

```cpp
void RenderTutorialOverlay(const SparkEditor::TutorialStep& step)
{
    switch (step.type)
    {
    case SparkEditor::TutorialStepType::HighlightPanel:
        HighlightEditorPanel(step.targetPanel);
        ShowTooltipNearPanel(step.targetPanel, step.message, step.tooltipPosition);
        break;

    case SparkEditor::TutorialStepType::ShowTooltip:
        ShowTooltipNearPanel(step.targetPanel, step.message, step.tooltipPosition);
        break;

    case SparkEditor::TutorialStepType::ShowMessage:
        ShowCenteredMessageDialog(step.message);
        break;

    case SparkEditor::TutorialStepType::WaitForAction:
        ShowTooltipNearPanel(step.targetPanel, step.message, step.tooltipPosition);
        ShowWaitingIndicator();
        break;

    case SparkEditor::TutorialStepType::WaitForInput:
        ShowCenteredMessageDialog(step.message);
        break;
    }
}
```

### Advancing Steps

Steps advance manually (user clicks "Next") or automatically (via `autoAdvanceDelay`):

```cpp
auto& tut = SparkEditor::TutorialSystem::GetInstance();

// Manual advance (e.g., user clicks "Next" button)
if (nextButtonClicked)
{
    tut.AdvanceStep();
}

// For WaitForAction steps, advance when the action is detected
if (tut.GetCurrentStep() &&
    tut.GetCurrentStep()->type == SparkEditor::TutorialStepType::WaitForAction)
{
    if (ActionWasPerformed(tut.GetCurrentStep()->requiredAction))
    {
        tut.AdvanceStep();
    }
}
```

### Querying Progress

```cpp
auto& tut = SparkEditor::TutorialSystem::GetInstance();

// Check if a tutorial is active
if (tut.IsTutorialActive())
{
    std::println("Running: {} (step {}/{})",
                 tut.GetCurrentTutorialName(),
                 tut.GetCurrentStepIndex() + 1,
                 tut.GetTotalSteps());
}

// Check completion status
if (tut.IsTutorialCompleted("Getting Started"))
{
    std::println("Getting Started tutorial has been completed");
}
```

## Built-in Tutorials

### Getting Started (Beginner)

Introduces the main SparkEditor interface:

| Step | Type | Target Panel | Message |
|------|------|-------------|---------|
| 1 | `ShowMessage` | -- | Welcome to SparkEditor! This tutorial will walk you through the main interface. |
| 2 | `HighlightPanel` | `Viewport` | This is the 3D Viewport where you see your scene. |
| 3 | `HighlightPanel` | `SceneHierarchy` | The Scene Hierarchy lists every entity in your scene. |
| 4 | `HighlightPanel` | `Inspector` | Select an entity to view its components here. |
| 5 | `ShowMessage` | -- | You are ready to start building! Explore the menus above. |

### Placing Entities (Beginner)

Teaches entity creation and positioning:

| Step | Type | Target Panel | Required Action |
|------|------|-------------|-----------------|
| 1 | `ShowMessage` | -- | -- |
| 2 | `HighlightPanel` | `SceneHierarchy` | -- |
| 3 | `WaitForAction` | `SceneHierarchy` | `CreateEntity` |
| 4 | `HighlightPanel` | `Inspector` | -- |
| 5 | `ShowMessage` | -- | -- |

### Material Setup (Intermediate)

Covers PBR material creation and assignment:

| Step | Type | Target Panel | Required Action |
|------|------|-------------|-----------------|
| 1 | `ShowMessage` | -- | -- |
| 2 | `HighlightPanel` | `AssetBrowser` | -- |
| 3 | `WaitForAction` | `AssetBrowser` | `CreateMaterial` |
| 4 | `HighlightPanel` | `MaterialEditor` | -- |
| 5 | `ShowMessage` | -- | -- |

## Creating Custom Tutorials

### Registering a New Tutorial

```cpp
SparkEditor::TutorialSequence seq;
seq.name = "Terrain Sculpting";
seq.description = "Learn how to sculpt terrain with brushes and layers.";
seq.difficulty = SparkEditor::TutorialDifficulty::Intermediate;

// Step 1: Intro message
seq.steps.push_back({
    SparkEditor::TutorialStepType::ShowMessage,
    {},                                            // no target panel
    "This tutorial covers terrain sculpting tools.",
    SparkEditor::TooltipPosition::Bottom,
    {},                                            // no required action
    0.f                                            // manual advance
});

// Step 2: Highlight the terrain panel
seq.steps.push_back({
    SparkEditor::TutorialStepType::HighlightPanel,
    "TerrainEditor",
    "Select a sculpting brush from this panel.",
    SparkEditor::TooltipPosition::Left,
    {},
    0.f
});

// Step 3: Wait for the user to select a brush
seq.steps.push_back({
    SparkEditor::TutorialStepType::WaitForAction,
    "TerrainEditor",
    "Click on a brush to select it.",
    SparkEditor::TooltipPosition::Left,
    "SelectBrush",  // action string to match
    0.f
});

// Step 4: Auto-advancing tip (3 seconds)
seq.steps.push_back({
    SparkEditor::TutorialStepType::ShowTooltip,
    "Viewport",
    "Now click and drag in the viewport to sculpt.",
    SparkEditor::TooltipPosition::Top,
    {},
    3.0f  // auto-advance after 3 seconds
});

// Step 5: Completion message
seq.steps.push_back({
    SparkEditor::TutorialStepType::ShowMessage,
    {},
    "Great work! You can now sculpt terrain.",
    SparkEditor::TooltipPosition::Bottom,
    {},
    0.f
});

auto& tut = SparkEditor::TutorialSystem::GetInstance();
tut.RegisterTutorial(std::move(seq));
```

### Step Types Guide

| Step Type | When to Use | Required Fields |
|-----------|-------------|----------------|
| `ShowMessage` | Display instructional text in a centered dialog | `message` |
| `HighlightPanel` | Draw attention to a specific editor panel | `targetPanel`, `message` |
| `ShowTooltip` | Show a tooltip near a panel without full highlight | `targetPanel`, `message`, `tooltipPosition` |
| `WaitForAction` | Block until user completes a specific action | `targetPanel`, `message`, `requiredAction` |
| `WaitForInput` | Block until any keyboard/mouse input is received | `message` |

### Auto-Advance Steps

Set `autoAdvanceDelay` to a positive value (in seconds) to automatically advance after a timer:

```cpp
TutorialStep timedStep;
timedStep.type = TutorialStepType::ShowTooltip;
timedStep.targetPanel = "Viewport";
timedStep.message = "Watch the animation play...";
timedStep.autoAdvanceDelay = 5.0f;  // Auto-advance after 5 seconds
```

## Configuration

### Callbacks

Register callbacks to be notified when the active step changes (useful for UI overlay updates):

```cpp
tut.OnStepChanged([](uint32_t stepIndex, const SparkEditor::TutorialStep& step) {
    std::println("Tutorial advanced to step {}: {}", stepIndex + 1, step.message);
    UpdateOverlayForStep(step);
});
```

### Navigation

Jump to any step or stop the tutorial:

```cpp
// Jump to step 3 (zero-based index)
tut.GoToStep(2);

// Stop the current tutorial without completing it
tut.StopTutorial();

// Manually mark a tutorial as completed
tut.MarkCompleted("Getting Started");
```

### Listing Available Tutorials

```cpp
auto tutorials = tut.GetAvailableTutorials();
for (auto name : tutorials)
{
    bool completed = tut.IsTutorialCompleted(name);
    std::println("{} {}", name, completed ? "(completed)" : "");
}
```

## Console Commands

```cpp
std::string status = tut.Console_GetStatus();
// Output: "[TutorialSystem] tutorials=3, completed=1, active=Getting Started, step=2/5"
```

## Integration

### With the Editor Panel System

Tutorial steps reference editor panels by name (e.g., `"Viewport"`, `"SceneHierarchy"`, `"Inspector"`, `"AssetBrowser"`, `"MaterialEditor"`). The rendering layer looks up the panel's screen position to place highlights and tooltips.

```cpp
// Example: panel highlight rendering
if (step.type == TutorialStepType::HighlightPanel)
{
    auto* panel = EditorPanelFactory::FindPanel(step.targetPanel);
    if (panel)
    {
        DrawHighlightRect(panel->GetBounds());
        DrawTooltip(step.message, panel->GetBounds(), step.tooltipPosition);
    }
}
```

### With the Editor Application

Initialize the tutorial system during editor startup and update each frame:

```cpp
void EditorApplication::Initialize()
{
    auto& tut = SparkEditor::TutorialSystem::GetInstance();
    tut.Initialize();  // Registers built-in tutorials

    // Show "Getting Started" on first launch
    if (!tut.IsTutorialCompleted("Getting Started"))
    {
        tut.StartTutorial("Getting Started");
    }
}

void EditorApplication::Shutdown()
{
    SparkEditor::TutorialSystem::GetInstance().Shutdown();
}
```

### With the Action System

`WaitForAction` steps listen for named actions. The editor's action system notifies the tutorial when an action occurs:

```cpp
void OnEditorAction(const std::string& action)
{
    auto& tut = SparkEditor::TutorialSystem::GetInstance();
    if (!tut.IsTutorialActive())
        return;

    const auto* step = tut.GetCurrentStep();
    if (step && step->type == TutorialStepType::WaitForAction &&
        step->requiredAction == action)
    {
        tut.AdvanceStep();
    }
}
```

## API Reference

### TutorialSystem (Singleton)

| Method | Description |
|--------|-------------|
| `GetInstance() -> TutorialSystem&` | Access the singleton |
| `Initialize()` | Clear state and register built-in tutorials |
| `Shutdown()` | Release all state and callbacks |
| `RegisterTutorial(TutorialSequence)` | Register a tutorial sequence |
| `StartTutorial(string_view) -> bool` | Start a tutorial by name |
| `StopTutorial()` | Stop the active tutorial |
| `AdvanceStep()` | Advance to the next step (completes if at end) |
| `GoToStep(uint32_t index)` | Jump to a specific step |
| `GetCurrentStep() -> const TutorialStep*` | Get the current step (nullptr if inactive) |
| `GetCurrentStepIndex() -> uint32_t` | Zero-based index of current step |
| `GetTotalSteps() -> uint32_t` | Total steps in active tutorial (0 if none) |
| `IsTutorialActive() -> bool` | Check if a tutorial is running |
| `GetCurrentTutorialName() -> string_view` | Name of active tutorial (empty if none) |
| `GetAvailableTutorials() -> vector<string_view>` | List all registered tutorial names |
| `IsTutorialCompleted(string_view) -> bool` | Check if a tutorial was completed |
| `MarkCompleted(string_view)` | Mark a tutorial as completed |
| `Update(float dt)` | Per-frame update for auto-advance timers |
| `OnStepChanged(function<void(uint32_t, const TutorialStep&)>)` | Register step-change callback |
| `Console_GetStatus() -> string` | Human-readable status for console |

## Thread Safety

- `TutorialSystem` is a singleton with **no internal synchronization**. All methods must be called from the **main/editor thread**.
- Step-changed callbacks are invoked synchronously during `AdvanceStep()`, `GoToStep()`, and `StartTutorial()`. Do not perform heavy work in callbacks.
- `GetInstance()` uses a function-local static and is safe for concurrent first-access under C++11 magic-statics guarantees.

## See Also

- [[Editor-Overview]] -- SparkEditor architecture and panel system
- [[Editor-Panels]] -- Complete list of editor panels (referenced by tutorial steps)
- [[Console-System]] -- Engine console for debugging tutorial state
- [[UI-System]] -- Underlying UI framework for tutorial overlays
