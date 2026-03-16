# Code Quality Violations — Functions, Methods, and Conventions

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** High

## Description

Comprehensive audit of CLAUDE.md hard limit violations beyond file/class size: oversized functions, private method limits, and naming conventions.

---

## 1. Functions Over 50 Lines (66 Violations)

CLAUDE.md: *"Writing a function longer than 50 lines"* is a sign of bloat.

### Top 15 Worst Offenders

| Function | File | Lines | Priority |
|----------|------|-------|----------|
| RegisterEngineConsoleCommands() | SparkEngine.cpp | 555 | P0 |
| RenderMainMenuBar() | EditorUI.cpp | 514 | P0 |
| main() | SparkEngine.cpp | 488 | P0 |
| RegisterHealthCommands() | SparkConsole.cpp | 453 | P0 |
| CreatePanels() | EditorUI.cpp | 412 | P0 |
| LoadFromFile() | MaterialSystem.cpp | 407 | P0 |
| RegisterDefaultCommands() | SparkConsole.cpp | 396 | P0 |
| CreateCombatArena() | Game.cpp | 379 | P0 |
| RegisterCrashCommands() | SparkConsole.cpp | 373 | P0 |
| Initialize() | Game.cpp | 364 | P0 |
| wWinMain() | SparkEngine.cpp | 330 | P1 |
| RegisterCameraCommands() | SparkConsole.cpp | 294 | P1 |
| RegisterPlayerCommands() | SparkConsole.cpp | 292 | P1 |
| RegisterGraphicsCommands() | SparkConsole.cpp | 263 | P1 |
| Console_ValidateMaterials() | MaterialSystem.cpp | 247 | P1 |

### By File

| File | Violations | Total Lines in Oversized Functions |
|------|-----------|-----------------------------------|
| SparkConsole.cpp | 29 | ~5,500 |
| GraphicsEngine.cpp | 16 | ~1,400 |
| MaterialSystem.cpp | 16 | ~1,800 |
| EditorUI.cpp | 11 | ~1,700 |
| Game.cpp | 9 | ~1,300 |
| SparkEngine.cpp | 5 | ~1,660 |
| **Total** | **66** | **~13,360** |

### Duplicate Functions Detected

- `CompileEmbeddedPixelShader` appears twice in GraphicsEngine.cpp (lines 2012, 4522)
- `CompileEmbeddedVertexShader` appears twice in GraphicsEngine.cpp (lines 1941, 4459)
- `GetShaderPermutation` appears twice in MaterialSystem.cpp (88 and 83 lines)
- `SaveToFile` appears twice in MaterialSystem.cpp (161 and 84 lines)

---

## 2. Private Helper Methods Over 10 (7 Class Violations)

CLAUDE.md: *"Private helper methods per class: 10 max."*

| Class | File | Private Methods | Over By |
|-------|------|----------------|---------|
| PostProcessingPipeline | PostProcessingPipeline.h | **84** | +74 (8.4x) |
| PlatformInputManager + backends | PlatformInput.h | **53** (distributed) | +43 |
| PhysicsSystem | PhysicsSystem.h | **42** | +32 (4.2x) |
| SimpleConsole | SparkConsole.h | **42** | +32 (4.2x) |
| RenderGraph | RenderGraph.h | **26** | +16 (2.6x) |
| WaterSystem | WaterSystem.h | **23** | +13 (2.3x) |
| BehaviorTree + nodes | BehaviorTree.h | **16+** | +6 |

---

## 3. Naming Convention Violations

CLAUDE.md: *"PascalCase classes/methods, camelCase locals, m_ prefix members, UPPER_SNAKE macros"*

### Struct Members Without m_ Prefix

Systematic across data-transfer structs. The `m_` prefix is consistently used for **class private members** but not for **public struct fields**. This is a gray area — POD structs commonly omit `m_`.

**Most affected files:**
- `Input/InputBindings.h` — InputBinding, InputPreset structs
- `Input/GamepadInput.h` — GamepadState struct (8 fields)
- `Input/PlatformInput.h` — Multiple event/binding structs (14+ fields)
- `Input/InputManager.h` — Stats/config structs (13+ fields)
- `Engine/Scripting/ScriptHotReload.h` — FileChangeEvent, RecompileResult, FileState, PendingChange (12+ fields)
- `Engine/Scripting/VisualScriptSystem.h` — Node/pin structs
- `Engine/Procedural/SplatmapSystem.h` — SplatmapLayer (11 fields)
- `Utils/SparkConsole.h` — LogEntry, CommandInfo, WatchEntry

**Clarification needed:** Whether `m_` applies to POD/data structs or only private class members.

### Method and Macro Naming

- Public methods are consistently PascalCase (no violations found)
- Macros are consistently UPPER_SNAKE (platform stubs like `_In_` are acceptable)

---

## 4. Commented-Out Code (Minimal)

CLAUDE.md: *"Commented-out code → delete it"*

Only 2 minor instances found:
- `SeamlessAreaManager.cpp:541-543` — 3 lines of planned SceneTransitionManager integration
- `SparkConsole.cpp:424-434` — 4 lines of incomplete game state assignments

The codebase is clean in this regard.

---

## Summary

| Violation Type | Count | Severity |
|---------------|-------|----------|
| Functions >50 lines | 66 | High |
| Duplicate functions in same file | 4 | High |
| Classes with >10 private methods | 7 | High |
| Struct m_ prefix violations | ~80+ fields | Low (gray area) |
| Commented-out code | 2 blocks | Low |
