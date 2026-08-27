---
name: sparkengine-build-ci-and-dependencies
description: >-
  SparkEngine build-system, CI, and third-party dependency runbook: CMake presets and the
  build-directory layout, MSVC toolset pinning (-T v143 / VS 2026 v145), compile/link flag
  policy, ENABLE_* feature-flag semantics and configure-time gating, ThirdParty/dependencies.lock
  manifest rules, the SDL2 package-export repair, the CI fresh-cache contract, install/export
  boundaries (find_package(SparkEngine)), and the GitHub Actions job matrix. TRIGGER when:
  "how do I build this", a cmake configure or generate fails, "which preset do I use",
  "which ENABLE_* flag gates which feature", a CI job fails (build-windows-vs2022,
  check-thirdparty-manifest, build-linux-*), adding/bumping a third-party dependency or
  submodule, "SDL_main.h not found", LNK2038 _ITERATOR_DEBUG_LEVEL, SparkEngineTargets
  export errors, package/CPack/install questions, or Release-only Windows misbehavior.
  DO NOT TRIGGER when: the question is how to run tests or interpret test failures (use
  sparkengine-validation-and-qa), how to launch/operate built binaries (use
  sparkengine-run-package-and-release), or a runtime gameplay/subsystem bug (use
  sparkengine-debugging-playbook).
---

# SparkEngine — Build, CI, and Dependencies

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

Runbook for configuring, building, and packaging SparkEngine, and for keeping the
third-party dependency manifest and CI green. All commands are repo-relative and were
verified on 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of commit `0e1fe7e7`).
The default upstream branch is **`Working`**, not `main`.

**Jargon, defined once:**
- **Preset** — a named configure/build recipe in `CMakePresets.json` (CMake ≥ 3.25).
- **Toolset** — the MSVC compiler generation selected with `-T` (`v143` = VS 2022, `v145` = VS 2026).
- **Export set** — the CMake target group (`SparkEngineTargets`) installed so external
  projects can `find_package(SparkEngine)`.
- **Manifest** — `ThirdParty/dependencies.lock`, the machine-checked inventory of every
  third-party dependency.

## When NOT to use this skill

| You actually want… | Use instead |
|---|---|
| Running/filtering `SparkTests`, ctest, sanitizer suppressions, flaky tests | `sparkengine-validation-and-qa` |
| Launching `SparkEngine.exe` / `SparkEditor.exe`, logs, crash dumps | `sparkengine-run-package-and-release` |
| A subsystem behaves wrongly at runtime | `sparkengine-debugging-playbook` |

(`ENABLE_*` feature-flag semantics — which toggle gates which feature — are owned by
this skill; grep the flag name in the root `CMakeLists.txt` and `CMakePresets.json`.)

## Toolchain requirements (enforced, not aspirational)

| Requirement | Where enforced |
|---|---|
| CMake ≥ 3.25 | `CMakeLists.txt:1`, `CMakePresets.json` `cmakeMinimumRequired` |
| C++23, no extensions | `CMakeLists.txt` (`CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_EXTENSIONS OFF`, `target_compile_features(... cxx_std_23)`) |
| MSVC ≥ 19.36 (VS 2022 17.6+) | `message(FATAL_ERROR ...)` if `MSVC_VERSION LESS 1936` — this is the "C++23 gate" |
| Out-of-source build only | `FATAL_ERROR` if `CMAKE_SOURCE_DIR == CMAKE_BINARY_DIR` |
| Linux: `libgl-dev` before configure when SDL2 builds from submodule | SDL2 config block warns and requires reconfigure from a deleted build dir |

## Build directory truth — three coexisting conventions

This repo has **three** configure styles that write to **different directories**. Know
which one you are in before running `cmake --build`:

| Style | Configure | Build dir | Build command |
|---|---|---|---|
| **Presets** | `cmake --preset windows-release` | `build/windows-release/` (`binaryDir` is `${sourceDir}/build/${presetName}`) | `cmake --build --preset windows-release` |
| **Raw `-B build`** (what CI and the CLAUDE.md snippets use) | `cmake -B build -G "Visual Studio 17 2022" -A x64 -T v143 -DBUILD_TESTS=ON` | `build/` | `cmake --build build --config Release --parallel` |
| **Wrapper scripts** | `./build.ps1 -config Release` / `./build.sh release` | `build/` (scripts `cd build; cmake ..`) | done by the script |

Gotchas (all verified):
- `cmake --build build` after a **preset** configure fails or builds a stale tree — the
  preset tree is `build/<presetName>`. Use `cmake --build --preset <name>` with presets.
- `build.ps1` defaults **`-DENABLE_EDITOR=OFF`**, `ENABLE_CONSOLE=OFF`,
  `ENABLE_ANGELSCRIPT=OFF` unless you pass `-editor -console -angelscript`. A plain
  `./build.ps1` build has no editor. `build.sh` is the opposite (all three default ON)
  and defaults to the **Ninja** generator.
- There is **no Ninja preset** in `CMakePresets.json` (verified: only VS, plain
  Makefile-style Linux/macOS, and MinGW presets). Ninja Multi-Config is supported by the
  build logic (per-config archive dirs are written for it) but you must pass
  `-G "Ninja Multi-Config"` manually.
- Binaries land in `<builddir>/bin` (plus `bin/<Config>` under multi-config VS),
  shared libs in `<builddir>/lib`, and **static archives in `<builddir>/lib/<Config>`**
  (see "Windows Release archaeology" below for why).

### Canonical commands

```bash
# Windows (MSVC, from any shell with cmake on PATH)
cmake --preset windows-release
cmake --build --preset windows-release
ctest --test-dir build/windows-release -C Release --output-on-failure --parallel

# Linux
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
cd build/linux-gcc-release && ./bin/SparkTests

# CI-equivalent raw configure (matches build-windows-vs2022 exactly)
cmake --fresh -B build -G "Visual Studio 17 2022" -A x64 -T v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --parallel
```

Other presets that exist (all in `CMakePresets.json`): `windows-debug`,
`windows-shipping` (MinSizeRel, dev tools off), `windows-development` (RelWithDebInfo +
`SPARK_NATIVE_ARCH=ON`), `linux-{gcc,clang}-{debug,release}`, `linux-{shipping,development}`,
`ci-linux-asan`, `ci-linux-tsan`, `linux-mingw-{release,debug}` (cross-compile via
`cmake/toolchains/mingw-w64-x86_64.cmake`), `macos-{debug,release,metal,moltenvk}`,
`minimal`. Presets are host-OS-conditioned: Windows presets are invisible on Linux and
vice versa.

## MSVC pinning rules

- Select the toolset **only** via the generator: `-T v143` (VS 2022) or the preset's
  `"toolset"` field. `SPARK_MSVC_TOOLSET` is a **deprecated compatibility hint**; if set
  and it disagrees with the generator toolset, configure **fails fatally** by design.
  Never rewrite generator toolset/platform from project code.
- VS 2026 uses CMake ≥ 4.2's native `-G "Visual Studio 18 2026"` generator with its
  **default v145 toolset** (no `-T` flag in the CI job). Note: the CLAUDE.md CI table
  says "MSVC v144" for this job — the workflow itself uses the v145 default; trust the
  workflow.
- Runtime library is pinned via CMP0091 to `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`
  (i.e. `/MD` / `/MDd`) for **all** targets including vendored deps.
- Debug info format is pinned via CMP0141 to `Embedded` (`/Z7`) so object files are
  cacheable by compiler caches while link-time `/DEBUG` still emits PDBs.

## Compile/link flag policy (top-level `CMakeLists.txt`, section 6/6.5)

| Area | MSVC | GCC/Clang |
|---|---|---|
| Warnings | `/W3 /MP /bigobj` + suppressions (`/wd4005 /wd4996 /wd4244 /wd4267 /wd26495`), `/permissive-`, `/Zc:__cplusplus`, `/Zc:preprocessor` | `-Wall -Wextra` + a suppression list (`-Wno-unused-parameter`, etc.) |
| Optimized configs | `/O2 /Oi /GL /GF /Gy /favor:AMD64`; link `/LTCG /OPT:REF /OPT:ICF` | `-ffunction-sections -fdata-sections -fstrict-aliasing`; link `--gc-sections` (`-dead_strip` on Apple) |
| LTO | via `/GL` + `/LTCG` (always on for non-Debug) | `ENABLE_LTO` option (ON): `-flto=auto` (GCC) / `-flto=thin` (Clang) in Release/MinSizeRel only |
| Debug | `/RTC1 /JMC /INCREMENTAL`, `_ITERATOR_DEBUG_LEVEL=1` | `-Og` (GCC, replacing `-O0`), `_GLIBCXX_ASSERTIONS` |
| Shipping (MinSizeRel) | `/O2` (speed over size) | `-Os` replaced with `-O3`; `--strip-all` |
| Arch | `SPARK_NATIVE_ARCH=ON` → `/arch:AVX2` | `SPARK_NATIVE_ARCH=ON` → `-march=native`; otherwise `-msse4.2` baseline |
| Hardening | `/GS` (default) | `-fstack-protector-strong`, `_FORTIFY_SOURCE=2` (non-Debug), full RELRO on Linux ELF |

**Note on /W3 vs /W4:** CLAUDE.md states "/W4 on MSVC" as the standard; the actual
enforced flag in the top-level `CMakeLists.txt` is `/W3` plus targeted suppressions
(verified line 212 at HEAD). Keep code /W4-clean where practical, but do not assume CI
enforces /W4.

## Windows Release-only reproducibility archaeology

Real, previously-hit Release-only failures whose fixes are load-bearing in the build:

1. **`/OPT:ICF` type-id folding.** `EngineContext`'s compile-time TypeId uses a
   `static char id;` per template instantiation. If written as `static const char id = 0`,
   MSVC's `/OPT:ICF` (identical-COMDAT folding, Release-only) folds every T's id to one
   address, collapsing all type ids. **Rule: type-id statics must be non-const.**
   Documented in `wiki/getting-started/Architecture-Overview.md` ("Type ID System").
2. **Archive clobbering → LNK2038 `_ITERATOR_DEBUG_LEVEL` storm.** Static/import libs
   are per-config link inputs; Debug and Release writing to a flat `build/lib` clobbered
   vendor archives (Jolt et al.). Fix: `CMAKE_ARCHIVE_OUTPUT_DIRECTORY_<CONFIG>` =
   `build/lib/<Config>` — set **before** every `add_subdirectory` so third-party targets
   inherit it. Runtime output stays flat (`bin/`) on purpose; targets link archives by
   target name so paths rewrite automatically. Do not "simplify" this back to flat.
3. **Stale cached CMake metadata.** CI restores the `build/` directory from
   `actions/cache`, so an old cache could retain a different generator platform/toolset.
   Fix (commit `0e1fe7e7`): Windows CI configures with **`cmake --fresh`** — regenerates
   CMake metadata while keeping restored object files. Reproduce locally when a cached
   tree misbehaves: `cmake --fresh -B build -G "Visual Studio 17 2022" -A x64 -T v143 -DBUILD_TESTS=ON`.
4. **Toolchain-drift archive rejection (Linux/macOS CI).** `apt`/`brew` compiler updates
   between cached runs change object ABI; the cached `lib*.a` then fails with
   "file format not recognized". CI jobs `build-linux-clang` and `build-macos` delete
   `lib*.a` (and Linux GCC deletes `*.pch`) before building — the `.o` files still come
   from ccache. If you see that linker error locally after a compiler upgrade:
   `find build -name 'lib*.a' -delete` then rebuild.

## Third-party dependencies

### Two sourcing mechanisms

- **Git submodules** (`.gitmodules`): miniz, Dear ImGui (`docking` branch), EnTT,
  AngelScript (codecat mirror), RecastNavigation, SDL2 (pinned to the **`SDL2` branch**
  of libsdl-org/SDL so Dependabot does not auto-bump to SDL3).
  First step for any missing-dependency symptom:
  ```bash
  git submodule update --init --recursive
  ```
- **Vendored snapshots** (committed files, no submodule): Jolt Physics v5.5.1,
  tinyobjloader, stb_image, cgltf, miniaudio, nlohmann/json v3.11.3, tinyexr v1.0.13
  (hardened, SHA-256-pinned in the manifest), zstd v1.5.6 wrapper,
  VulkanMemoryAllocator, glad 0.1.36. curl is optional and found from the system
  (`find_package(CURL QUIET)`); it is not in the manifest.

All third-party **targets are created in the root `CMakeLists.txt` only** ("single
source of truth") and consumed elsewhere. Every dependency has a stub/fallback path
(headless NullRHI when SDL2 missing, DDS-only textures without stb, etc.); only Jolt and
miniz are configure-time **ERROR** severity.

### The manifest: `ThirdParty/dependencies.lock`

CMake-includable file defining `SPARK_THIRDPARTY_AUDIT_ENTRIES`. Each entry is one
pipe-delimited string with **exactly 9 fields**:

```
name|source|version_or_commit|license|local_path|required_files_csv|feature_macro|fallback_or_stub_path|severity
```

`severity` must be `ERROR` or `WARN`. The manifest is audited at every configure
(`spark_thirdparty_audit(...)` near the top of `CMakeLists.txt`), and since `0e1fe7e7`
a malformed entry is a **fatal** configure error (schema validation in
`cmake/SparkThirdPartyAudit.cmake`).

**When you touch any dependency wiring, you must update the manifest in the same
change**, including its `# Last sync:` header line. The CI job `check-thirdparty-manifest`
fails if any of these changed without a manifest change:
- anything under `ThirdParty/`
- diff lines in `.gitmodules`, `CMakeLists.txt`, or `cmake/SparkThirdPartyAudit.cmake`
  matching `ThirdParty/`, `https://`, `.git`, `SPARK_HAS_*`, `SPARK_RECAST_AVAILABLE`,
  `SPARK_JOLT_PHYSICS_AVAILABLE`

Run the exact CI check locally (also validates schema via CMake script mode):

```bash
./tools/check-thirdparty-manifest-sync.sh
```

Schema-only validation without git-drift logic:

```bash
cmake -DSPARK_THIRDPARTY_AUDIT_VALIDATE_ONLY=ON \
      -DSPARK_THIRDPARTY_MANIFEST="$PWD/ThirdParty/dependencies.lock" \
      -P cmake/SparkThirdPartyAudit.cmake
```

### SDL2: export repair and header-race fix (current at HEAD)

Two deliberate, load-bearing pieces of SDL2 wiring in the root `CMakeLists.txt` — do not
remove either:

1. **`SDL2_DISABLE_INSTALL OFF` (FORCE), commit `34ee7ab7`.** `SparkEngineLib`'s
   installed target links `SDL2::SDL2`. SDL disables its own install/export rules when
   embedded as a subproject, which left the vendored SDL2 target outside every export
   set — CMake then **rejects the `SparkEngineTargets` export at generate time**.
   Forcing SDL's install rules on ships SDL's own package with the SDK so installed
   consumers can resolve the dependency. If you see an error about a target in export
   set `SparkEngineTargets` not being exportable, this is the pattern to check.
2. **Configure-time public-header copy.** SDL2 normally copies its headers via a
   build-time custom target (`sdl_headers_copy`). Consumers that inherit SDL's include
   path without a build-order dependency (cached CI restores, tests including `<SDL.h>`
   through `SparkEngineLib`'s PUBLIC includes) can race it and die with
   `fatal error: SDL_main.h: No such file or directory`. The root CMakeLists copies
   `ThirdParty/SDL2/include/*.h` into `build/ThirdParty/SDL2/include/SDL2/` at
   **configure** time (excluding `SDL_config`/`SDL_revision`, which are generated).

SDL2 resolution order at configure (root CMakeLists, `ENABLE_SDL2` block): submodule
target → `find_package(SDL2)` (system) → pkg-config → warn and continue headless. Every
successful path sets `SPARK_SDL2_AVAILABLE` and `SPARK_PACKAGE_HAS_SDL2=1`, and links
with `$<BUILD_INTERFACE:...>` / `$<INSTALL_INTERFACE:SDL2::SDL2>` generator expressions
so the build tree and the installed package resolve SDL2 differently but correctly.
`ENABLE_SDL2` defaults **ON on non-Windows, OFF on Windows**.

## Package / export boundary (`find_package(SparkEngine)`)

What ships in the `sdk` install component (root `CMakeLists.txt`, install section):

- **Targets:** `SparkEngineLib` plus whichever of
  `miniz tinyexr zstd tinyobjloader stb_image cgltf miniaudio nlohmann_json glad Jolt Recast Detour`
  exist as targets — all in export set `SparkEngineTargets`, namespace **`Spark::`**,
  installed to `lib/cmake/SparkEngine/SparkEngineTargets.cmake`.
- **Headers:** `SparkSDK/Include/Spark` → `include/`; all engine `*.h/*.hpp` →
  `include/SparkEngine/`; curated third-party headers → `include/SparkEngine/ThirdParty/`.
- **Config files:** `SparkEngineConfig.cmake` (generated from
  `cmake/SparkEngineConfig.cmake.in`), `SparkEngineConfigVersion.cmake`
  (`SameMajorVersion`, version `1.0.0` via `SPARK_ENGINE_VERSION`), and
  `cmake/SparkGameModule.cmake` (provides `spark_add_game_module()` for external games).
- The config file emits `find_dependency(SDL2 REQUIRED)` / `find_dependency(CURL REQUIRED)`
  only when the build actually linked them (`SPARK_PACKAGE_HAS_SDL2` / `_CURL`).
- Consumers link `Spark::SparkEngineLib`.
- Install components: `runtime`, `sdk`, `tools`, `templates`, `samples`. CPack config:
  `cmake/SparkCPack.cmake` + `cmake/SparkCPackOptions.cmake`.

### Package smoke test — run this after touching install/export rules

`Tests/PackageSmoke/` is a minimal external consumer (`find_package(SparkEngine REQUIRED)`
+ link `Spark::SparkEngineLib`). CI runs it in `release.yml` ("Validate external package
consumption") — **release workflow only, not per-PR**. The canonical local reproduction
recipe lives in `sparkengine-validation-and-qa` §7 (do not duplicate it here).

If the export set is broken (e.g. SDL2 dropped out of it), the failure appears at the
**generate step of the main build** or at `--install`, before the smoke test even runs.

## CI matrix (`.github/workflows/build.yml`, verified at HEAD)

Triggers: push to `main`/`develop`/`Working`/`feature/**`/`claude/**`, PRs to
`main`/`develop`/`Working`, manual dispatch. Concurrency cancels superseded runs.

| Job | What / gate |
|---|---|
| `check-format` | clang-format `--dry-run --Werror` over engine+editor+modules sources (Metal dirs excluded); greps for actual violations because clang-format 18 mis-exits |
| `validate-prompts` | `./tools/validate-prompts.sh --ci` |
| `check-thirdparty-manifest` | `./tools/check-thirdparty-manifest-sync.sh` (fetch-depth 0) — **blocking** |
| `build-linux-gcc` / `build-linux-clang` | gcc-14 / clang, Debug+Release, ccache + cached build dir, runs `./bin/SparkTests`; GCC Release also asserts `ENABLE_VULKAN:BOOL=ON` in the cache and that two Vulkan-parity tests appear in the log |
| `build-linux-asan` / `-tsan` | GCC Debug with ASan+UBSan+LSan / TSan, suppression files in `Tests/` |
| `build-linux-msan` | Clang + libc++, `continue-on-error` (uninstrumented system libc++ ⇒ false positives), builds only `SparkTests` |
| `build-windows-vs2022` | windows-2022, Debug+Release, `cmake --fresh` configure (`-T v143`), sccache action installed, ctest on Release, packages Release zip |
| `build-windows-vs2026` | `continue-on-error`; vswhere **requires** VS `[18.0,19.0)` — an absent toolchain fails visibly instead of a green no-op; `-G "Visual Studio 18 2026"` (default v145) |
| `build-linux-mingw-wine` | **`workflow_dispatch` only** at HEAD (the CLAUDE.md table predates this), `continue-on-error`; MinGW toolchain file + Wine/DXVK run via `tools/wine-run.sh` |
| `build-macos` | `continue-on-error`; matrix: OpenGL Debug/Release + Metal Release; deletes cached `lib*.a` pre-build |
| `coverage` | gcc-14 `--coverage` + lcov, per-subsystem thresholds via the coverage-report script (repo `scripts/` dir), PR comment |
| `clang-tidy` | **blocking job** (no `continue-on-error`); warnings inside it are advisory (`--warnings-as-errors=""`), configure/compile failures fail it; first 30 engine .cpp files. Gate fine print: `sparkengine-validation-and-qa` §10 |
| annotation-count job (`to-do-count`, job id spelled without the inner hyphen) | warns above 20 to-do/fix-me/HACK-style markers; always exits 0 |
| `report-ci-errors` | aggregates `ci-errors-*` artifacts into one PR comment/summary |
| `build-installer` | standalone SparkInstaller (no engine) per-OS |

Advisory (job-level `continue-on-error: true`; never block merges):
`build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, `build-linux-msan`.
`clang-tidy` is **not** in this set — it is a blocking job (warnings inside it are
advisory, but configure/compile failures block). Canonical required-vs-advisory
fine print: `sparkengine-validation-and-qa` §10.

**Cache contract:** every build job restores `~/.ccache` (or sccache on Windows) *and*
the `build/` directory keyed on `hashFiles('**/CMakeLists.txt', '**/*.cmake')` + SHA.
The defenses against stale caches are: `cmake --fresh` (Windows), `*.pch` deletion
(Linux GCC), `lib*.a` deletion (Linux Clang, macOS). If you add a job that caches
`build/`, copy the matching defense. `open`: the vs2022 job installs sccache and prints
its stats, but its configure line passes no `CMAKE_*_COMPILER_LAUNCHER` — whether MSVC
compilations actually route through sccache there is unverified; don't cite its hit rate
as evidence.

Local reproduction commands for every job:
`wiki/development/CI-Reproducible-Builds.md`. Post-PR polling:
`gh pr checks --watch --fail-fast`, then `gh run view <RUN_ID> --log-failed`.

Release/publishing (`.github/workflows/release.yml`): nightly cron + `release/**`
branches + `v*` tags; builds Windows/Linux packages via CPack, runs the PackageSmoke
validation, publishes GitHub Releases and updates download badges.

## Quick triage table

| Symptom | Cause → Fix |
|---|---|
| `fatal error: SDL_main.h: No such file or directory` | Header-copy race with stale/partial build tree → reconfigure (configure-time copy repopulates `build/ThirdParty/SDL2/include/SDL2/`); if using a cached tree, `cmake --fresh` |
| CMake generate error: target not in export set `SparkEngineTargets` | A dependency linked into `SparkEngineLib`'s install interface without install rules → follow the SDL2 pattern (`SDL2_DISABLE_INSTALL OFF` precedent) |
| `LNK2038: _ITERATOR_DEBUG_LEVEL` mismatch | Debug/Release archives mixed → verify archives live under `build/lib/<Config>`; nuke `build/lib` and rebuild both configs |
| `error adding symbols: file format not recognized` on a `lib*.a` | Compiler upgraded under a cached tree → `find build -name 'lib*.a' -delete` and rebuild |
| Configure fails "In-source builds are not allowed" | You ran `cmake .` in the repo root → use `-B build` or a preset |
| Configure fails on MSVC version | VS older than 2022 17.6 (MSVC 19.36) → upgrade; the C++23 gate is fatal by design |
| `check-thirdparty-manifest` CI failure | Dependency wiring changed without touching `ThirdParty/dependencies.lock` → update the entry + `# Last sync:` line; run `./tools/check-thirdparty-manifest-sync.sh` |
| CI Windows job builds with wrong platform/toolset | Stale cached CMake metadata → the `--fresh` configure should prevent this; if editing workflows, keep `--fresh` |
| All EngineContext type-ids collapse (Release only) | `/OPT:ICF` folded const statics → type-id statics must be non-const (`static char id;`) |

## Provenance and maintenance

Verified 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`
"fix(ci): invalidate stale CMake metadata") by reading the cited files — no full
build/CI run at this exact tree. SDL2 export repair landed in `34ee7ab7`
("fix(ci): export SDL2 and format hardened sources"). Facts most likely to
drift: preset list, CI job set/gating, dependency pins, the sccache-launcher open item.

Re-verify with:

```bash
# Presets and binaryDir convention
grep -n 'binaryDir\|"name"' CMakePresets.json | head -30
# MSVC gate, toolset hint, runtime/debug-format pinning
grep -n 'MSVC_VERSION LESS 1936\|SPARK_MSVC_TOOLSET\|CMAKE_MSVC_RUNTIME_LIBRARY\|CMAKE_MSVC_DEBUG_INFORMATION_FORMAT' CMakeLists.txt
# Flag policy anchors
grep -n '/W3 /MP\|OPT:ICF\|_ITERATOR_DEBUG_LEVEL\|ENABLE_LTO\|SPARK_NATIVE_ARCH' CMakeLists.txt | head
# SDL2 export + header copy
grep -n 'SDL2_DISABLE_INSTALL\|sdl_headers_copy\|_SDL2_PUBLIC_HEADERS' CMakeLists.txt
# Export/install boundary
grep -n 'SparkEngineTargets\|SPARK_INSTALL_COMPONENT\|configure_package_config_file' CMakeLists.txt | head
# Manifest schema + audit
head -8 ThirdParty/dependencies.lock && ./tools/check-thirdparty-manifest-sync.sh
# CI fresh-cache contract and job gating
grep -n 'cmake --fresh\|continue-on-error\|workflow_dispatch\|lib\*.a' .github/workflows/build.yml | head -20
# Package smoke path
cat Tests/PackageSmoke/CMakeLists.txt
# Submodule pins
git submodule status
```
