# Selection Manager

Centralized editor object selection and picking pipeline with single, multi, marquee, and GPU-based selection modes.

**Source:** `SparkEditor/Source/Panels/SelectionManager.h`

## Overview

The Selection Manager is the single source of truth for which entities are selected in the editor. All panels -- Hierarchy, Inspector, Scene View, Property Grid -- observe selection changes through registered callbacks rather than polling, ensuring consistent state across the entire editor UI.

The system supports multiple selection paradigms: single-click replaces the selection, Ctrl+click adds or toggles individual entities, and marquee (box) selection captures all entities within a screen-space rectangle. GPU picking integrates with an ID render pass where each entity is drawn with a unique color; the clicked pixel's entity ID is fed back through `SetPickResult`. All selection operations respect the active filter and entity lock state.

Selection groups allow saving and restoring named sets of entities, useful for repeatedly working with the same objects. Entity locking prevents accidental selection of background geometry or locked layers. The callback system uses incrementing IDs so listeners can unregister cleanly.

## Key Classes

| Class / Struct | Description |
|---|---|
| `SelectionManager` | Singleton managing current selection state, marquee, filtering, groups, and callbacks |
| `SelectionChangedEvent` | Event payload listing added, removed, and current entities |
| `SelectionFilter` | Enum of preset filters: All, StaticMeshOnly, LightsOnly, CamerasOnly, PhysicsOnly, AIOnly, Custom |
| `MarqueeRect` | Screen-space rectangle with normalized bounds and point containment test |
| `SelectionGroup` | Named collection of entity IDs for save/restore |
| `ScreenPosition` | 2D screen coordinate for entity picking |

## Usage

```cpp
auto& sel = SparkEditor::SelectionManager::GetInstance();
sel.Initialize();

// Listen for selection changes (e.g., from Inspector panel)
auto cbId = sel.OnSelectionChanged([](const SparkEditor::SelectionChangedEvent& e) {
    for (auto id : e.added)
        RefreshInspector(id);
});

// Single select
sel.Select(entityId);

// Multi-select (Ctrl+Click)
sel.AddToSelection(entity2);
sel.ToggleSelection(entity3);

// Marquee selection (mouse drag in scene view)
sel.BeginMarquee(mouseStartX, mouseStartY);
sel.UpdateMarquee(mouseCurX, mouseCurY);
sel.EndMarquee(visibleEntityScreenPositions, /*additive=*/false);

// GPU picking from ID render pass
sel.SetPickResult(entityAtPixel, /*additive=*/holdingCtrl);

// Filter to lights only
sel.SetFilter(SparkEditor::SelectionFilter::LightsOnly);

// Lock background geometry
sel.LockEntity(groundPlaneId);

// Save/restore selection groups
sel.SaveSelectionGroup("BuildingInterior");
sel.RestoreSelectionGroup("BuildingInterior");

// Cleanup
sel.RemoveCallback(cbId);
```

## API Reference

### Selection Operations

| Method | Return | Description |
|---|---|---|
| `Select(entityId)` | `void` | Select a single entity, clearing previous selection |
| `AddToSelection(entityId)` | `void` | Add an entity to the current selection |
| `RemoveFromSelection(entityId)` | `void` | Remove an entity from the selection |
| `ToggleSelection(entityId)` | `void` | Toggle an entity's selected state |
| `ClearSelection()` | `void` | Deselect all entities |
| `SelectMultiple(entities)` | `void` | Replace selection with a list of entities |

### Query

| Method | Return | Description |
|---|---|---|
| `IsSelected(entityId)` | `bool` | Check if an entity is currently selected |
| `GetSelectionCount()` | `size_t` | Number of selected entities |
| `GetSelection()` | `const vector<EntityId>&` | Ordered list of selected entity IDs |
| `GetPrimarySelection()` | `EntityId` | The most recently clicked entity |
| `HasSelection()` | `bool` | Whether anything is selected |

### Marquee and Picking

| Method | Return | Description |
|---|---|---|
| `BeginMarquee(x, y)` | `void` | Start a marquee rectangle at screen position |
| `UpdateMarquee(x, y)` | `void` | Update the marquee corner as the mouse moves |
| `EndMarquee(positions, additive)` | `void` | Finalize marquee and select contained entities |
| `SetPickResult(entityId, additive)` | `void` | Feed GPU pick result into the selection system |

### Filtering and Locking

| Method | Return | Description |
|---|---|---|
| `SetFilter(filter)` | `void` | Set a selection filter preset |
| `SetCustomFilter(callback)` | `void` | Set a custom filter function |
| `ClearFilter()` | `void` | Remove all filters |
| `LockEntity(id)` | `void` | Prevent an entity from being selected |
| `UnlockEntity(id)` | `void` | Allow a locked entity to be selected again |

### Groups and Callbacks

| Method | Return | Description |
|---|---|---|
| `SaveSelectionGroup(name)` | `void` | Save current selection as a named group |
| `RestoreSelectionGroup(name)` | `void` | Restore a saved selection group |
| `DeleteSelectionGroup(name)` | `void` | Delete a named group |
| `OnSelectionChanged(callback)` | `uint32_t` | Register a change listener, returns callback ID |
| `RemoveCallback(callbackId)` | `void` | Unregister a change listener |

## Related Systems

- [Editor Panels](Editor-Panels.md) -- all panels observe selection via callbacks
- [Scene Hierarchy](Scene-Hierarchy.md) -- tree view drives and reflects selection
- [Inspector Panel](Inspector-Panel.md) -- displays properties of selected entities
- [Editor Automation](Editor-Automation.md) -- automation commands can manipulate selection
