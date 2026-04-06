# Header Namespace Issues

**Type:** Observation  
**Status:** Resolved  
**Date:** 2026-04-05

## Summary

Four header-level issues prevented clean inclusion of certain subsystem headers from files that also included `EngineContext.h`. All have been resolved.

## Issue 1: AnimationSystem Forward-Declaration vs Using-Alias Conflict — FIXED

**Files:** `IEngineContext.h`, `AnimationSystem.h`

**Problem:** `IEngineContext.h` forward-declared `class AnimationSystem` but the actual definition was `using AnimationSystem = AnimationManager`.

**Fix:** Changed `IEngineContext.h` to forward-declare `AnimationManager` and add the using-alias, matching the actual definition. Removed the redundant forward-declaration from `EngineSetup.h`.

## Issue 2: NetworkManager Namespace Mismatch — FIXED

**Files:** `IEngineContext.h`, `NetworkManager.h`

**Problem:** `IEngineContext.h` forward-declared `Spark::NetworkManager` but the class lives in `Spark::Net::NetworkManager`.

**Fix:** Added `namespace Net { class NetworkManager; }` and `using NetworkManager = Net::NetworkManager;` in the `Spark` namespace in `IEngineContext.h`. Removed the redundant forward-declaration from `EngineSetup.h`.

## Issue 3: PostProcessingTypes.h / PostProcessingEffects.h Duplicate Definitions — FIXED

**Files:** `PostProcessingTypes.h`, `PostProcessingEffects.h`

**Problem:** Both files defined identical enums and structs (PostProcessPass, FXAASettings, etc.).

**Fix:** Replaced `PostProcessingEffects.h` body with `#include "PostProcessingTypes.h"`, making `PostProcessingTypes.h` the canonical home.

## Issue 4: NetworkInterpolation.h / PacketValidator.h Namespace Scoping — NO ACTION NEEDED

Investigation showed these files already have proper `Spark::Net::` namespace qualification. The original concern was unfounded.
