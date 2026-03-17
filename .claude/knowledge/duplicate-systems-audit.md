# Duplicate Systems Audit — ODR Risks and Parallel Implementations

**Last updated:** 2026-03-17
**Type:** Observation
**Status:** Mostly Resolved

## Description

5 confirmed cases of duplicate or parallel system implementations. 3 ODR risks fixed, 1 architectural duplication documented, 1 cosmetic.

## Critical: ODR Violation Risks

### 1. AudioMixer — Two Classes, Same Name, Same Namespace — RESOLVED

**Fix applied:** MusicManager's AudioMixer was renamed to `AudioBusMixer` in a prior session.

### 2. AnimationStateMachine — Defined in Two Headers — RESOLVED

**Fix applied:** The standalone `AnimationStateMachine.h` header was deleted in a prior session. The canonical definition lives in `AnimationSystem.h`.

### 3. Dual EventBus Implementations — RESOLVED (2026-03-17)

**Fix applied:** `EventSystem.h` no longer defines its own `Spark::EventBus` class. It now includes `Utils/EventBus.h` (the canonical implementation with RAII SubscriptionHandle, per-type mutex, snapshot-based publish) and only defines built-in event types + QueuedEventBus. A `SubscriptionID` type alias is kept for backward compatibility.

### 4. SimpleConsole — Engine and Editor Versions

| File | Lines | Namespace |
|------|-------|-----------|
| `SparkEngine/Source/Utils/SparkConsole.h` | 162 | `Spark::SimpleConsole` |
| `SparkEditor/Source/Utils/SparkConsole.h` | 77 | `SparkEditor::SimpleConsole` |

Different namespaces prevent ODR violation. The engine version is now lean (551 lines .cpp, 162 lines .h) after refactoring in a prior session.

## Resolved: Visual Scripting — DELETED

Both VisualScriptSystem and VisualScriptingSystem (7,343 lines combined) were deleted as dead code.

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
| Dual EventBus implementations | High | **RESOLVED** (EventSystem.h delegates to EventBus.h) |
| SimpleConsole duplication | Low | Documented, different namespaces, engine version refactored |
| Dual VisualScripting systems | N/A | **RESOLVED** (both deleted as dead code) |
| SceneManager naming | Medium | OPEN (cosmetic) |
