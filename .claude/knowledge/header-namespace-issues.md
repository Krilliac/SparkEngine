# Header Namespace Issues

**Type:** Observation  
**Status:** Active  
**Date:** 2026-04-05

## Summary

Three header-level issues prevent clean inclusion of certain subsystem headers from files that also include `EngineContext.h`. These are pre-existing structural problems, not bugs in the current build — the engine compiles and runs correctly because no existing code triggers the conflicts.

## Issue 1: AnimationSystem Forward-Declaration vs Using-Alias Conflict

**Files:**
- `SparkSDK/Include/Spark/IEngineContext.h:38` — forward-declares `Spark::Animation::AnimationSystem` as a **class**
- `SparkEngine/Source/Engine/Animation/AnimationSystem.h:521` — defines `using AnimationSystem = AnimationManager` (a using-alias)

**Problem:** If a .cpp includes both `EngineContext.h` (which transitively includes `IEngineContext.h`) and `AnimationSystem.h`, the compiler sees `class AnimationSystem` followed by `using AnimationSystem = AnimationManager`, which is a conflicting declaration.

**Fix:** In `IEngineContext.h`, change the forward declaration from:
```cpp
namespace Spark::Animation { class AnimationSystem; }
```
to:
```cpp
namespace Spark::Animation { class AnimationManager; using AnimationSystem = AnimationManager; }
```
Or, rename the `AnimationManager` class to `AnimationSystem` and remove the using-alias entirely.

## Issue 2: NetworkManager Namespace Mismatch

**Files:**
- `SparkSDK/Include/Spark/IEngineContext.h:75` — forward-declares `Spark::NetworkManager`
- `SparkEngine/Source/Engine/Networking/NetworkManager.h:326` — defines `Spark::Net::NetworkManager`

**Problem:** `GetNetwork()` returns `Spark::NetworkManager*` but the actual class lives in `Spark::Net::NetworkManager`. These are different types in different namespaces. No existing code calls `GetNetwork()->Console_*()` so the mismatch has never caused a linker error, but it prevents writing code that does.

**Fix:** Add a using-alias in the `Spark` namespace after the `Spark::Net` namespace closes:
```cpp
namespace Spark { using NetworkManager = Net::NetworkManager; }
```

## Issue 3: PostProcessingTypes.h / PostProcessingEffects.h Duplicate Definitions

**Files:**
- `SparkEngine/Source/Graphics/PostProcessingTypes.h` — defines `PostProcessPass`, `FXAASettings`, etc.
- `SparkEngine/Source/Graphics/PostProcessingEffects.h` — also defines the same `PostProcessPass`, `FXAASettings`, etc.

**Problem:** Both files define the same enums and structs. Including both (e.g., `PostProcessingPipeline.h` includes `PostProcessingEffects.h`, and you also include `PostProcessingTypes.h`) causes "redefinition" errors despite `#pragma once`.

**Fix:** Pick one file as the canonical home for these types and have the other `#include` it. `PostProcessingTypes.h` is the better home (it was designed for lightweight inclusion). Then `PostProcessingEffects.h` should `#include "PostProcessingTypes.h"` instead of redefining the types.

## Issue 4: NetworkInterpolation.h / PacketValidator.h Internal Namespace Scoping

**Files:**
- `SparkEngine/Source/Engine/Networking/NetworkInterpolation.h` — uses `InterpolationSnapshot` without qualifying `Spark::Net::InterpolationSnapshot`
- `SparkEngine/Source/Engine/Networking/PacketValidator.h` — uses `PacketViolation`, `MessageType`, `MessageSchema` without proper namespace qualification

**Problem:** These headers use types from `Spark::Net` without full qualification. They only compile when included from within a `namespace Spark::Net { }` block or after a `using namespace Spark::Net;`. Including them from outside the namespace (like from `Core/`) causes "does not name a type" errors.

**Fix:** Add `Spark::Net::` qualification to all type references in these headers, or add `using` declarations at the top of each header.

## Workaround

Until these are fixed, code in `Core/` that needs to call `Console_*` methods on Animation, Network, or PostProcessing should:
1. Use `EngineSettings::SetValue()` / `GetValue()` for settings-based control (works now)
2. Use singleton accessors directly (e.g., `Spark::Net::NetworkManager::GetInstance()`) instead of `EngineContext::GetNetwork()`
3. Avoid including both `EngineContext.h` and `AnimationSystem.h` in the same translation unit
