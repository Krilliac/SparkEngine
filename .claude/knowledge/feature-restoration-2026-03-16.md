# Feature Restoration — Wrongly Deleted Legitimate Features

**Last updated:** 2026-03-16
**Type:** Decision
**Status:** Resolved

## Description

Prior cleanup sessions deleted ~40K lines of "orphaned dead code." Review revealed that some deleted features were legitimate, complete implementations that just needed to be wired in — not stubs or dead code.

## Context

The anti-bloat rules say "Features built but not integrated count as bugs, not WIP" and mandate deletion. However, the correct action for *complete, tested implementations* is to wire them in, not delete them. The distinction matters:
- **Stub**: Empty or placeholder implementation that does nothing useful → DELETE
- **Orphan**: Complete implementation that just wasn't instantiated → WIRE IN

## Features Restored

### Editor Panels (wired into EditorUI.cpp CreatePanels() + Window menu)

| Panel | Lines | Why Restored |
|-------|-------|-------------|
| **HierarchyPanel** | 1,045+338 | Full tree hierarchy with drag-drop, undo, multi-select. *Replaced* inferior SimpleHierarchyPanel (283 lines, flat list, hardcoded objects). |
| **MaterialEditorPanel** | 1,832+270 | PBR material editor with 12 shader presets, property editing, preview. Essential for any 3D engine editor. |
| **PlayModeToolbarPanel** | 644+81 | Play/Stop/Pause controls with time scale, subsystem toggles. Essential for play-in-editor workflow. PlayModeManager dependency still exists. |

### Engine Systems (restored with tests, added to CMakeLists.txt)

| System | Lines | Why Restored |
|--------|-------|-------------|
| **LocalizationSystem** | 227+197 | Complete file-based string table with regex JSON parsing, per-language loading. Has passing tests. |
| **DestructionSystem** | 147+267 | Complete fracture pattern system with 3 built-in presets (wood, metal, concrete). Has passing tests. |

## What Was Correctly Deleted (confirmed stubs/dead code)

These were genuinely dead and correctly removed:
- 20 header-only Graphics stubs (no .cpp, never included)
- Visual Scripting (2 separate copies, neither wired in)
- Procedural Generation (52KB but stub-only Update())
- ContentDelivery (HTTP CDN framework with no HTTP implementation)
- SeamlessAreaManager, SceneTransitionManager (never wired in)
- 8 editor panels that were never created (DialogueEditor, AssetDependency, AudioMixer, etc.)

## Key Learning

**Deletion vs wiring decision tree:**
1. Does the system have tests? → Wire it in (someone invested effort in testing)
2. Does it have a real implementation (not `(void)param; return false;`)? → Wire it in
3. Does it depend on something that still exists? → Wire it in
4. Is it a stub with no real logic? → Delete it

## Notes

- HierarchyPanel needed 3 new public methods (CreateObject, ResetToDefault, GetSceneObjects) for EditorUI compatibility
- HierarchyPanel creates an owned SceneFile on Initialize() so it works standalone without external SetScene()
- SimpleHierarchyPanel was deleted after replacement
- Both restored engine systems had their tests re-added to Tests/CMakeLists.txt
