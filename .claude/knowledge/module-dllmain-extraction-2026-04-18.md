# Module DllMain Extraction (SparkSDK ModuleDllMain.h)

**Last updated:** 2026-04-18
**Type:** Pattern
**Status:** Active

## Description

The 15-line Windows `DllMain` boilerplate that every game module DLL needed has been extracted to a single shared header `SparkSDK/Include/Spark/ModuleDllMain.h`. All 10 GameModules (`SparkGame`, `SparkGameFPS`, `SparkGameMMO`, `SparkGameRPG`, `SparkGameARPG`, `SparkGameRTS`, `SparkGameRacing`, `SparkGamePlatformer`, `SparkGameOpenWorld`, `SparkGameVisualScript`) plus the `EmptyProject` template now include this header instead of hand-rolling their own `DllMain`.

## Context

- Applies to any new game module DLL authored against `Spark/IModule` + `SPARK_IMPLEMENT_MODULE(...)`.
- Also applies to docs/specs/templates that show the module entry point pattern.

## Approach

In one `.cpp` per module DLL (typically `Main.cpp` alongside `SPARK_IMPLEMENT_MODULE`):

```cpp
#include "MyGame.h"
#include <Spark/ModuleRegistry.h>
#include <Spark/ModuleDllMain.h>   // <-- emits the canonical DllMain on _WIN32, no-op elsewhere

SPARK_IMPLEMENT_MODULE(MyGameModule)
```

The header defines `extern "C" BOOL APIENTRY DllMain(...)` that calls `DisableThreadLibraryCalls` on `DLL_PROCESS_ATTACH` and returns `TRUE`. It guards `<windows.h>` with `WIN32_LEAN_AND_MEAN` + `NOMINMAX` to keep symbol pollution minimal.

**Do not** include this header in more than one TU per DLL — the linker will (correctly) reject duplicate `DllMain` symbols.

## Details

- Extraction saved ~150 LOC across the 10 GameModule `Main.cpp` files (15 lines × 10).
- Single change point for DllMain policy going forward (e.g., if we want per-thread setup, ASAN hooks, or Detours install, we edit one file).
- Prior to this, the 4 broken templates (`FPSStarter`, `MultiplayerArena`, `PlatformerKit`, `RPGStarter`) used a non-existent `SPARK_REGISTER_MODULE` macro — they never compiled. That's a **separate** bug not addressed here; when those templates are fixed to use `SPARK_IMPLEMENT_MODULE`, they should also add `#include <Spark/ModuleDllMain.h>`.
- Docs updated:
  - `GameModules/README.md`
  - `docs/specs/plugin-abi-guide.md`
  - `wiki/getting-started/Creating-a-Game-Module.md`

## Solution / Summary

Prefer `#include <Spark/ModuleDllMain.h>` over rolling a per-module `DllMain`. Include it exactly once per DLL, next to `SPARK_IMPLEMENT_MODULE(...)`. Header is a no-op off Windows so it does not need platform guards at the call site.

## Notes

- The header intentionally emits a function definition (not a macro) so that the one-DllMain-per-DLL invariant is enforced by the linker rather than silently elided.
- If a module ever needs custom `DllMain` logic (e.g., per-thread TLS setup), that module should *not* include `ModuleDllMain.h` — write its own `DllMain` directly and accept the per-module maintenance cost. The shared header is the "I want the default" path.
- Related: `SparkSDK/Include/Spark/ModuleRegistry.h` (`SPARK_IMPLEMENT_MODULE`, `SPARK_MODULE_DEPENDENCIES`).
