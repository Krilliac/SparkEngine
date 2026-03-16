# Duplicate Systems Audit — ODR Risks and Parallel Implementations

**Last updated:** 2026-03-16
**Type:** Observation
**Status:** Partially Resolved
**Severity:** Medium (was Critical — 2 of 3 ODR risks fixed)

## Description

5 confirmed cases of duplicate or parallel system implementations. 2 ODR risks have been fixed. 1 architectural duplication remains.

## Critical: ODR Violation Risks

### 1. AudioMixer — Two Classes, Same Name, Same Namespace — RESOLVED

**Fix applied:** MusicManager's AudioMixer was renamed to `AudioBusMixer` in a prior session.

### 2. AnimationStateMachine — Defined in Two Headers — RESOLVED

**Fix applied:** The standalone `AnimationStateMachine.h` header was deleted in a prior session. The canonical definition lives in `AnimationSystem.h`.

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

## Medium: Parallel Visual Scripting — RESOLVED (deleted)

### 5. VisualScriptSystem vs VisualScriptingSystem — DELETED

Both systems (7,343 lines combined) were deleted as dead code in March 2026. Neither was wired into any startup path or included by any other source file. The orphaned test was also removed.

## Medium: SceneManager Naming Collision

| File | Namespace | Purpose |
|------|-----------|---------|
| `SceneManager/SceneManager.h` | `Spark::SceneManager` | Runtime scene hierarchy |
| `SparkEditor/SceneSystem/SceneManager.h` | `SparkEditor::SceneManager` | Editor scene file management |

Different namespaces, but identical class names cause confusion when reading code.

## Summary

| Issue | Severity | Status |
|-------|----------|--------|
| AudioMixer ODR risk | Critical | **RESOLVED** (renamed to AudioBusMixer) |
| AnimationStateMachine ODR risk | Critical | **RESOLVED** (standalone header deleted) |
| Dual EventBus implementations | High | OPEN (merge best of both) |
| SimpleConsole duplication | High | Documented, bridge exists |
| Dual VisualScripting systems | N/A | **RESOLVED** (both deleted as dead code) |
| SceneManager naming | Medium | OPEN (cosmetic) |
