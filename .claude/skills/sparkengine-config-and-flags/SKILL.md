---
name: sparkengine-config-and-flags
description: >
  Catalog of every SparkEngine CMake build option (ENABLE_*/SPARK_*/BUILD_*), its real
  verified default, the C/C++ #define it produces, what code it gates, and the CMakePresets
  it is set by. TRIGGER when the user asks "what does ENABLE_VULKAN / ENABLE_EDITOR / ENABLE_DXR
  do", "how do I turn off networking / the editor / tests", "which preset builds shipping",
  "why is my #ifdef SPARK_* not firing", "what flag maps to which define", or "how do I add a
  new build toggle correctly". DO NOT TRIGGER for MSVC toolchain setup, vcvars, sccache, or
  build-error triage (use the global cmake-msvc skill instead) — this skill is only the
  project's feature-flag surface, not how to run the compiler.
trilobite_compatible: true
trilobite_role: reference
---

# SparkEngine config & flags catalog

SparkEngine's build surface is driven entirely from the **root `D:\SparkEngine\CMakeLists.txt`**
plus **`D:\SparkEngine\CMakePresets.json`**. This skill is the ground-truth map of that surface:
which `option()` exists, its verified default, the compile `#define` it emits, and what it gates.

> **Vocabulary** — *option*: a CMake cache boolean/string declared with `option()` or `set(... CACHE ...)`.
> *compile definition* / *define*: a `-D` / `/D` macro (e.g. `SPARK_VULKAN_SUPPORT`) that C++ code tests
> with `#ifdef`. In this project an option name (`ENABLE_VULKAN`) and the define it produces
> (`SPARK_VULKAN_SUPPORT`) are usually **different strings** — do not assume they match.

## When NOT to use this skill

- **Toolchain / build-error problems** (cl.exe not found, `CC=claude.exe`, sccache misses, MSVC
  Debug caching) → use the global **cmake-msvc** skill. This skill never covers *how to run* the build.
- **Adding a new subsystem/source file** → that's an anti-bloat / architecture question, not a flag.
- You only need the *command* to configure a preset → `cmake --preset <name>` then
  `cmake --build build --config <cfg>`; the preset list is in the table below.

---

## 1. How a flag becomes a `#define` (the plumbing)

Three distinct mechanisms exist in the root `CMakeLists.txt`. Know which one a flag uses before
you touch it:

| Mechanism | Where | Example |
|-----------|-------|---------|
| `list(APPEND FEATURE_DEFINITIONS ...)` → applied `PUBLIC` to `SparkEngineLib` at the end | section 14, ~lines 1916-2027 | `ENABLE_PROFILING` → `PROFILING_ENABLED` |
| Direct `target_compile_definitions(SparkEngineLib PUBLIC ...)` at point of use | scattered, ~lines 1341-1902 | `ENABLE_VULKAN` → (probe) → `SPARK_VULKAN_SUPPORT` |
| `add_subdirectory()` gating only — **no define**, just includes/excludes a whole target | ~lines 1403-1499 | `ENABLE_EDITOR`, `ENABLE_LAUNCHER` |

`SparkEngineLib` is the static core; the `SparkEngine` executable and every game-module DLL inherit
its `PUBLIC` defines. On Linux/macOS game modules link the header-only `SparkEngineInterface` target,
which re-exports `SparkEngineLib`'s compile definitions (root CMakeLists ~line 1129).

---

## 2. Feature flags — verified catalog

Every row below was read from `D:\SparkEngine\CMakeLists.txt` on **2026-07-07**. "Default" is the
literal value in the `option()` call, **not** what CLAUDE.md summarizes (a few differ — see notes).

| Option | Verified default | Emits define | Gates / effect |
|--------|------------------|--------------|----------------|
| `ENABLE_GRAPHICS` | `ON` | *(none)* | **Declared but INERT** — no `if(ENABLE_GRAPHICS)` and no define consumes it anywhere in the tree. Toggling it does nothing today. (CLAUDE.md lists it among working toggles; this row documents verified CMake reality, which differs from the manifest.) |
| `ENABLE_PROFILING` | `ON` | `PROFILING_ENABLED` | Profiler instrumentation code paths. |
| `ENABLE_NETWORKING` | `ON` | `ENABLE_NETWORKING` (+ `SPARK_HAS_CURL` if libcurl found) | UDP sockets, AreaServer/WorldServer, crash-report upload. |
| `ENABLE_VULKAN` | `ON` | `SPARK_VULKAN_SUPPORT` (only if Vulkan SDK is actually found) | Vulkan RHI backend + links VMA. Experimental. Absent SDK → silently off. |
| `ENABLE_OPENGL` | `ON` | `SPARK_OPENGL_SUPPORT` (needs OpenGL **and** GLAD); `SPARK_EGL_SUPPORT` if EGL found | OpenGL RHI backend + GLAD loader; EGL enables Linux headless software rendering. Experimental. |
| `ENABLE_METAL` | `${APPLE}` → **ON on macOS, OFF elsewhere** | `SPARK_METAL_SUPPORT` | Metal RHI backend (macOS only, experimental). CLAUDE.md's "OFF" is only true off-macOS. |
| `ENABLE_DXR` | `ON` | `SPARK_HARDWARE_RT` (only when `ENABLE_HYBRID_RT` **and** `WIN32` **and** not MinGW) | Hardware DirectX Raytracing via D3D12. Off Windows / on MinGW → SDFGI software fallback. |
| `ENABLE_HYBRID_RT` | `ON` | `SPARK_HYBRID_RT` | SDFGI software GI + optional hardware RT layer. |
| `ENABLE_SDL2` | `ON` on non-Windows, `OFF` on Windows | `SPARK_SDL2_AVAILABLE` (if an SDL2 target/pkg resolves) | Cross-platform windowing + input. On Linux, built from `ThirdParty/SDL2` submodule. |
| `SPARK_HEADLESS_SUPPORT` | `ON` | `SPARK_HEADLESS_SUPPORT` | Headless / dedicated-server mode (NullRHIDevice path). |
| `ENABLE_RECAST` | `ON` | `SPARK_RECAST_AVAILABLE` (only if `ThirdParty/AI/recastnavigation` present) | Recast/Detour voxel navmesh builder. |
| `ENABLE_NEURAL_RENDERING` | `ON` | `SPARK_NEURAL_RENDERING=1` | NTC / radiance cache / neural post-processing. When OFF, the define is dropped (primary gate). |
| `ENABLE_VR` | `OFF` | `SPARK_VR_ENABLED` | VR/AR OpenXR-ready stub. When OFF the define is absent and `VR/VRSystem.cpp` is filtered out. |
| `ENABLE_MOBILE` | `OFF` | `SPARK_MOBILE_ENABLED` | Touch/gesture/battery-aware scaling. When OFF, `Mobile/MobilePlatform.cpp` is filtered out. |
| `BUILD_TESTS` | `ON` | *(none)* | Adds `Tests/` (CTest suite) via `add_subdirectory`. |
| `BUILD_GAME_MODULES` | `ON` | *(none)* | Builds in-tree `GameModules/*` DLLs. Set OFF for engine-only builds. |
| `SPARK_STRICT_DEPS` | `OFF` | *(none)* | When ON, missing Jolt / ImGui / EnTT becomes `FATAL_ERROR` instead of a warning. |
| `SPARK_SUPPRESS_THIRDPARTY_WARNINGS` | `ON` | *(none)* | Applies `/w` (MSVC) / `-w` to Jolt and other third-party targets. |
| `SPARK_DOUBLE_PRECISION_PHYSICS` | `OFF` | forces Jolt `DOUBLE_PRECISION` → `JPH_DOUBLE_PRECISION` | Double-precision physics for large worlds (MMO/open-world). |
| `ENABLE_EDITOR` | `ON` | *(none)* | Adds `SparkEditor` (ImGui editor). Auto-skips if ImGui / (Linux) SDL2+OpenGL deps are missing. |
| `ENABLE_LAUNCHER` | `ON` | *(none)* | Builds `SparkLauncher` (project picker). |
| `ENABLE_SPARKBUILD` | `ON` | *(none)* | Builds `SparkBuild` (terminal-UI CMake configurator). |
| `ENABLE_INSTALLER` | `ON` | *(none)* | Builds `SparkInstaller` (needs `SparkBuildCore` from SparkBuild). |
| `ENABLE_CONSOLE_IN_SHIPPING` | `OFF` | `SPARK_CONSOLE_IN_SHIPPING` | Include SparkConsole in Shipping (`MinSizeRel`) builds. |
| `ENABLE_DEVCOMMANDS_IN_SHIPPING` | `OFF` | `SPARK_DEVCOMMANDS_IN_SHIPPING` | Include dev cheats (god, noclip, ...) in Shipping. |
| `STRIP_DEBUG_SYMBOLS` | `OFF` | `SPARK_STRIP_DEBUG_SYMBOLS` (+ linker strip in MinSizeRel) | Strip symbols from the final binary. |
| `SPARK_NATIVE_ARCH` | `OFF` | *(none)* | ON → `/arch:AVX2` (MSVC) or `-march=native`. **Do not ship** native binaries. |
| `ENABLE_LTO` | `ON` | *(none)* | **Non-MSVC only** (declared inside the GCC/Clang branch, ~line 349). Enables `-flto`. On MSVC, LTO (`/GL`+`/LTCG`) is always on for non-Debug and this option does not exist. |
| `SPARK_MSVC_TOOLSET` | `""` (empty = auto; `v143` default on Windows) | *(none)* | Pins MSVC toolset. `v143`=VS2022, `v144`=VS2026. |
| `SPARK_ENGINE_VERSION` | `"1.0.0"` | *(package metadata)* | Semantic version for packaging. |

### Config-derived defines (automatic, not options)

The build type auto-emits exactly one profile define (root CMakeLists ~lines 2018-2023):

| Build type | Define |
|------------|--------|
| `Debug` | `SPARK_BUILD_DEBUG` |
| `RelWithDebInfo` | `SPARK_BUILD_DEVELOPMENT` |
| `Release` | `SPARK_BUILD_RELEASE` |
| `MinSizeRel` (Shipping) | `SPARK_BUILD_SHIPPING` |

Other always-on platform/compiler defines: `SPARK_PLATFORM_WINDOWS` / `SPARK_PLATFORM_LINUX` /
`SPARK_PLATFORM_MACOS`, and `SPARK_COMPILER_VS2022` / `VS2026` / `VS2019` by `_MSC_VER`. Third-party
availability defines (`SPARK_JOLT_PHYSICS_AVAILABLE`, `SPARK_HAS_IMGUI`, `SPARK_MINIZ_AVAILABLE`,
`SPARK_ZSTD_AVAILABLE`, `SPARK_HAS_STB_IMAGE`, `SPARK_HAS_CGLTF`, `SPARK_HAS_MINIAUDIO`,
`SPARK_HAS_NLOHMANN_JSON`, `SPARK_HAS_TINYEXR`, `SPARK_OPENAL_AVAILABLE`) are set from presence probes,
not options — you cannot force them on; provide the submodule instead.

### MinGW cross-compile special case

Building under MinGW (`linux-mingw-*` presets) excludes the D3D12 + DXR sources and defines
`SPARK_NO_D3D12` (headers too old); D3D11 remains the primary backend under Wine + DXVK.

---

## 3. Phantom options in presets (WARNING)

`CMakePresets.json` sets several cache variables that **no `option()` declares and no CMake logic
reads** — they are inert no-ops. Verified 2026-07-07: zero matches in any `CMakeLists.txt`.

- In the `minimal` preset: `ENABLE_AI`, `ENABLE_ANIMATION`, `ENABLE_SAVE_SYSTEM`,
  `ENABLE_PROCEDURAL`, `ENABLE_CINEMATIC`, `ENABLE_DECALS`, `ENABLE_MESH_LOD`.
- In `windows-shipping` / `linux-shipping` / `*-development`: `ENABLE_HOT_RELOAD`.

**Do not cite these as working toggles.** Setting `-DENABLE_AI=OFF` changes nothing today. If you
actually want them to gate code, you must first declare the `option()` and wire a define/exclusion
(see section 5). Treat them as `candidate` (aspirational), not shipped behaviour.

---

## 4. Presets catalog (`cmake --preset <name>`)

Verified from `CMakePresets.json`. Windows presets require host Windows; `linux-*` require Linux;
`macos-*` require Darwin (the `condition` blocks enforce this).

| Preset | Build type | Notable non-default cache vars |
|--------|-----------|-------------------------------|
| `windows-debug` | Debug | `SPARK_MSVC_TOOLSET=v143`, VS 2022 generator |
| `windows-release` | Release | `SPARK_MSVC_TOOLSET=v143`, VS 2022 generator |
| `windows-development` | RelWithDebInfo | `SPARK_NATIVE_ARCH=ON`, console + dev commands ON |
| `windows-shipping` | MinSizeRel | editor/profiling OFF, `STRIP_DEBUG_SYMBOLS=ON`, `BUILD_TESTS=OFF`, `SPARK_NATIVE_ARCH=OFF` |
| `linux-gcc-debug` / `linux-gcc-release` | Debug / Release | gcc/g++ |
| `linux-clang-debug` / `linux-clang-release` | Debug / Release | clang/clang++ |
| `linux-development` | RelWithDebInfo | gcc, `SPARK_NATIVE_ARCH=ON`, console + dev commands ON |
| `linux-shipping` | MinSizeRel | gcc, editor/profiling OFF, strip ON, tests OFF |
| `ci-linux-asan` | Debug (inherits linux-gcc-debug) | `-fsanitize=address,undefined` |
| `ci-linux-tsan` | Debug (inherits linux-gcc-debug) | `-fsanitize=thread` |
| `linux-mingw-release` / `linux-mingw-debug` | Release / Debug | MinGW toolchain file; `ENABLE_VULKAN/OPENGL/SDL2=OFF` |
| `macos-debug` / `macos-release` | Debug / Release | `ENABLE_METAL=ON`, `ENABLE_OPENGL=ON`, `ENABLE_VULKAN=OFF`, `ENABLE_DXR=OFF`, `ENABLE_SDL2=ON` |
| `macos-metal` | Release | Metal ON, Vulkan OFF |
| `macos-moltenvk` | Release | Vulkan ON (via MoltenVK), Metal OFF |
| `minimal` | Release | `ENABLE_NETWORKING=OFF`, `ENABLE_DXR=OFF` + several **phantom** vars (see section 3) |

Override any option on top of a preset:
`cmake --preset windows-release -DENABLE_NETWORKING=OFF -DENABLE_EDITOR=OFF`

---

## 5. Runbook — add a new `ENABLE_` toggle correctly

Follow every step; a toggle wired in only one place is worse than none (silent no-op like the
phantom options above).

1. **Declare the option** in root `CMakeLists.txt` section 3 (near lines 117-151):
   ```cmake
   option(ENABLE_MYFEATURE "One-line description of what this gates" OFF)
   ```
   Pick the default deliberately: `ON` only if the feature is production-ready and its deps are
   always present.

2. **Emit a define** so C++ can `#ifdef` it. Prefer the `FEATURE_DEFINITIONS` block (section 14,
   ~line 1916) so it is applied `PUBLIC` to `SparkEngineLib` in one place:
   ```cmake
   if(ENABLE_MYFEATURE)
       list(APPEND FEATURE_DEFINITIONS "SPARK_MYFEATURE_ENABLED")
   endif()
   ```
   (Name the define distinctly — `SPARK_MYFEATURE_ENABLED`, not `ENABLE_MYFEATURE`.) If the feature
   is a whole optional target instead of `#ifdef`s, gate its `add_subdirectory()` like `ENABLE_EDITOR`
   (~line 1469) and skip the define.

3. **Guard the C++** with the *define*, never the option name:
   ```cpp
   #ifdef SPARK_MYFEATURE_ENABLED
       // feature code
   #endif
   ```

4. **(Optional) exclude sources when OFF** — mirror the VR/Mobile pattern (~lines 1939-1946). Note:
   in the current tree these `list(FILTER SPARK_ENGINE_LIB_SOURCES ...)` calls run *after*
   `add_library(SparkEngineLib ...)` (~line 1099), so the **define is the load-bearing gate**;
   treat source exclusion as best-effort, and if you need a hard exclusion, place the filter
   *before* the `add_library`.

5. **(Optional) set it in presets** — add to `CMakePresets.json` only for presets that should
   differ from the option default (e.g. turn it OFF in `windows-shipping`). Setting it to its own
   default value is noise.

6. **Verify it is not a no-op** before committing:
   ```bash
   # option is declared
   grep -n 'option(ENABLE_MYFEATURE' CMakeLists.txt
   # a define is actually produced
   grep -rn 'SPARK_MYFEATURE_ENABLED' CMakeLists.txt
   # C++ actually reads the define
   grep -rn 'SPARK_MYFEATURE_ENABLED' SparkEngine/Source
   ```
   All three must return hits. (This is exactly the check that exposes the section-3 phantom options.)

7. **Docs** — per CLAUDE.md, adding a build toggle is a code change: run
   `docs/update-all-docs.sh` and add the flag to the Build section of `D:\SparkEngine\CLAUDE.md`.

---

## 6. Quick answers

- "Turn off networking" → `-DENABLE_NETWORKING=OFF` (drops `ENABLE_NETWORKING` define + curl).
- "Headless server build" → keep `SPARK_HEADLESS_SUPPORT=ON` (default); optionally `-DENABLE_EDITOR=OFF -DENABLE_GRAPHICS`-is-inert (use `ENABLE_VULKAN/OPENGL/DXR=OFF` to trim GPU backends).
- "Ship build" → `cmake --preset windows-shipping` (MinSizeRel, no editor/console/tests, stripped).
- "My `#ifdef SPARK_X` never fires" → confirm the *option* that should emit it is ON **and** its
  dependency probe succeeded (Vulkan/OpenGL/Recast/curl defines only appear if the lib is found).
- "Double-precision physics for big worlds" → `-DSPARK_DOUBLE_PRECISION_PHYSICS=ON`.

---

## Provenance and maintenance

- **Verified 2026-07-07** against `D:\SparkEngine\CMakeLists.txt` (2643 lines) and
  `D:\SparkEngine\CMakePresets.json`. Line numbers are approximate; grep by name if they drift.
- Re-verify option defaults:
  `grep -nE 'option\((ENABLE_|SPARK_|BUILD_)' D:/SparkEngine/CMakeLists.txt`
- Re-verify define mappings:
  `grep -nE 'FEATURE_DEFINITIONS|target_compile_definitions' D:/SparkEngine/CMakeLists.txt`
- Re-detect phantom preset options (should list only options NOT declared by `option()`):
  compare `CMakePresets.json` keys against `grep -oE 'ENABLE_[A-Z_]+' CMakeLists.txt`.
- Re-list presets: `cmake --list-presets` (from `D:\SparkEngine`).
- Volatile facts most likely to rot: the phantom-option set (section 3) and `ENABLE_GRAPHICS`
  being inert — re-run the section-5 step-6 checks if a flag's behaviour is in doubt.
- Related skill: **cmake-msvc** (global) for toolchain / build-execution issues — this skill does
  not cover running the compiler.
