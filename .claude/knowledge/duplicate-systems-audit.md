# Duplicate Systems Audit — ODR Risks and Parallel Implementations

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Active
**Severity:** Critical

## Description

5 confirmed cases of duplicate or parallel system implementations. 3 carry ODR (One Definition Rule) violation risk. 2 are architectural duplications that fragment functionality.

## Critical: ODR Violation Risks

### 1. AudioMixer — Two Classes, Same Name, Same Namespace

| File | Pattern |
|------|---------|
| `Audio/AudioMixer.h` (331 lines) | Instance-based; Initialize()/Update() |
| `Audio/MusicManager.h` line 57 (part of 292 lines) | Singleton; SetBusVolume/FadeBus |

Both define `class AudioMixer` in `namespace Spark::Audio`. Including both headers in one translation unit is an ODR violation.

**Fix:** Rename MusicManager's AudioMixer to `AudioBusMixer`.

### 2. AnimationStateMachine — Defined in Two Headers

| File | Context |
|------|---------|
| `Engine/Animation/AnimationStateMachine.h` (108+ lines) | Standalone dedicated header |
| `Engine/Animation/AnimationSystem.h` (line 560+) | Embedded within AnimationSystem |

Same class defined in both files. Including both causes ODR violation.

**Fix:** Remove the definition from AnimationSystem.h; keep only in AnimationStateMachine.h and `#include` it.

### 3. SimpleConsole — Engine and Editor Versions

| File | Lines | Namespace |
|------|-------|-----------|
| `SparkEngine/Source/Utils/SparkConsole.h` | 450 | `Spark::SimpleConsole` |
| `SparkEditor/Source/Utils/SparkConsole.h` | 77 | `SparkEditor::SimpleConsole` |

Different namespaces prevent ODR violation, but the duplication is bridged via `EditorConsoleBridge.h` (documented as "Problem R7.3"). Separate command registries and log histories cause behavioral divergence.

## High: Parallel Event Systems

### 4. EventBus — Two Independent Implementations

| File | API Pattern | Thread Safety |
|------|------------|---------------|
| `Utils/EventBus.h` (lines 146-315) | RAII `SubscriptionHandle` (auto-unsubscribe) | Mutex-protected |
| `Engine/Events/EventSystem.h` (lines 65-182) | Manual `SubscriptionID` (integer-based) | Mutex-protected |

Both implement type-safe pub/sub using `std::type_index`. EventSystem.h also defines `QueuedEventBus` for deferred dispatch. Code can use either, fragmenting event flow.

**Fix:** Pick one. EventBus.h has better RAII semantics; EventSystem.h has deferred dispatch. Merge the best of both into one.

## Medium: Parallel Visual Scripting

### 5. VisualScriptSystem vs VisualScriptingSystem (7,991 lines combined)

| System | Location | Lines |
|--------|----------|-------|
| VisualScriptSystem | `Engine/Scripting/VisualScriptSystem.{h,cpp}` | 3,054 |
| VisualScriptingSystem | `SparkEditor/VisualScripting/VisualScriptingSystem.{h,cpp}` | 4,937 |

Both define node graphs, pin types, compilation, serialization. Neither includes the other. Neither is wired into startup. Both are effectively dead code.

## Medium: SceneManager Naming Collision

| File | Namespace | Purpose |
|------|-----------|---------|
| `SceneManager/SceneManager.h` | `Spark::SceneManager` | Runtime scene hierarchy |
| `SparkEditor/SceneSystem/SceneManager.h` | `SparkEditor::SceneManager` | Editor scene file management |

Different namespaces, but identical class names cause confusion when reading code.

## Summary

| Issue | Severity | Fix Effort |
|-------|----------|-----------|
| AudioMixer ODR risk | Critical | 15 min (rename) |
| AnimationStateMachine ODR risk | Critical | 15 min (remove duplicate) |
| Dual EventBus implementations | High | 1-2 hours (merge) |
| SimpleConsole duplication | High | Documented, bridge exists |
| Dual VisualScripting systems | High | Multi-session refactor |
| SceneManager naming | Medium | Rename one |
