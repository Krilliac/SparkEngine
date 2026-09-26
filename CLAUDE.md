# SparkEngine — Claude Code Context

## What is this?

SparkEngine is a C++23 open-source 3D game engine (with C++26 forward-compatibility macros). Originally focused on first-person shooters, it is evolving into a general-purpose engine supporting FPS, RPG, MMO, open-world, and other genres.
- **Rendering**: Full RHI abstraction — D3D11 (primary), D3D12/Vulkan/Metal/OpenGL (experimental backends)
- **Physics**: Jolt Physics
- **Audio**: XAudio2
- **ECS**: EnTT
- **Scripting**: AngelScript (with hot-reload and client/server context separation)
- **Editor**: Dear ImGui (with collaborative multi-user editing)
- **Networking**: UDP client/server, AreaServer/WorldServer architecture (HeroEngine-inspired)
- **Large worlds**: Floating-point origin rebasing, seamless area streaming
- **Headless/Software rendering**: NullRHIDevice fallback (no GPU) or full CPU rendering via OpenGL + Mesa llvmpipe
- **Primary platform**: Windows 10+ (MSVC); Linux/macOS are experimental (macOS has CI job + CMake presets)

Engine systems architecture is the priority, and only production-ready code lands. MSVC is the primary CI target, and the cross-platform builds must stay green.

## Session start (run at the beginning of every session)

**Step 1 — Git sync** (see [Git Sync Workflow](#git-sync-workflow) below for the commands):

Sync your branch with the latest upstream `Working` branch. This is the **first thing** to do — before reading code, before making changes, before anything else. Feature branches diverge as other PRs merge; without rebasing you'll be working on stale code.

**Step 2 — Load persistent context:**

```bash
cat wiki/_Sidebar.md
```

Project knowledge lives in the **wiki** (the `.claude/knowledge` base was retired 2026-06-08). Scan the sidebar — especially the **Development & Process**, **Research & Analysis**, and **Engineering Notes & Audits** sections — for pages relevant to the current task, and read those before proceeding.

**Step 3 — Bloat check:**

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
  -name '*.cpp' | xargs wc -l | sort -rn | head -15
```

If the task involves any file over the threshold, trim it first.

## Anti-Bloat Guidelines

AI-assisted development has a structural bias toward complexity: adding features "just in case," creating helpers for single uses, over-engineering simple problems, building systems without wiring them in. The goal is **sanity, not sacrifice** — keep code clean without stripping legitimate verbosity or readability.

### Sensible Thresholds (Not Hard Limits)

These are **guidelines for when to pause and think**, not absolute rules. A clean 450-line `.cpp` is fine; a cryptic 200-line `.cpp` is not.

| Thing | Threshold | What to do |
|-------|-----------|------------|
| `.cpp` file size | ~500 lines | Split if doing multiple jobs; leave if one coherent unit |
| `.h` file size | ~300 lines | Split if unrelated types; data-heavy headers are fine |
| Public methods per class | ~15 | Ask: "Does each method earn its place?" |
| Function length | ~60 lines | Split if nested branching; clear linear flow is fine |
| Command registration functions | 1 per subsystem | Consolidate before adding commands |
| Parallel singleton systems doing the same thing | 0 | Remove the duplicate |

### The Readability Principle

**Never sacrifice readability to hit a line count.** Keep comments that explain "why," use descriptive variable names (`brushRadius` > `br`), maintain vertical whitespace between logical sections, use braces for non-trivial loop bodies, and one statement per line. The question is always: **"Does this make sense to someone reading it for the first time?"**

### Before Writing Code — Checklist

1. **Does this already exist?** Search before writing.
2. **Will this be called?** If you can't name the caller, don't write it.
3. **Can existing code do this with a small change?** Prefer editing over adding.
4. **Is this a one-time use?** Inline it — no helper function, no new class.
5. **Am I future-proofing?** Stop. Write only what is needed today.
6. **Adding a new class/file?** Ask if an existing one can be extended instead.
7. **Adding new command registrations?** They go in ONE place per subsystem.
8. **Is the code dead?** Delete it. Don't comment it out — git history exists.
9. **Is a system built but not wired in?** Either wire it in or delete it.

## Coding Standards

**Ground truth first.** Read the build config before assuming the C++ standard, compiler flags, platforms or graphics API: the root `CMakeLists.txt`, `CMakePresets.json`, `cmake/` and `.github/workflows/build.yml` (the build is CMake only, with no premake). Match the existing conventions, `.clang-format` and `.clang-tidy`, and do not introduce a new style.

- **C++23**: `constexpr`, `enum class`, structured bindings, `std::format`, `std::expected`, `std::print`, concepts, deducing `this`, `if consteval`, `std::unreachable`
- **Ownership**: `std::unique_ptr` owning, raw pointers non-owning. No naked `new`/`delete`
- **RAII**: D3D11 via `ComPtr`, all resources released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` prefix members, `UPPER_SNAKE` macros
- **Headers**: `#pragma once`, forward-declare where possible
- **Style**: Allman braces, 4-space indent, 120-col limit (see `.clang-format`)
- **Warnings**: root MSVC flags are `/W3 /MP /bigobj` (CMakeLists.txt; no `/W4`, no `/WX`), GCC/Clang use `-Wall -Wextra`; CI does not fail on warnings. Keep new code warning-free at those levels -- a zero-warning build is a goal, not an enforced gate
- **Service locator**: Use `EngineContext::Get()->GetX()` for subsystem access. Engine-lifetime ownership lives in the `EngineRuntime` struct (Core-internal; `Core/EngineRuntime.h`) — do not introduce new file-scope `g_*` subsystem globals
- **Cross-platform types**: `Core/Platform.h` (DirectXMath stubs on Linux)

### Language policy

Exceptions and RTTI are both ON. No flag disables them; CMake's MSVC defaults `/EHsc /GR` apply.

- **Exceptions** are for unrecoverable, initialization and tooling failures. Per-frame and hot-path code stays non-throwing and `noexcept`, and expected failures there return result types (`std::expected`, `bool` + out-param) instead of throwing.
- **Move constructors and move assignment are `noexcept`.** Containers depend on it: `std::vector` copies instead of moving on reallocation otherwise. `performance-noexcept-move-constructor` flags misses (advisory).
- **No `dynamic_cast` or `typeid` in per-frame paths.** Both are fine in editor, tools and serialization code.

### Engine systems

- **Every system states its contract** in its header docs:
  - thread affinity (game thread, render thread, or async-safe);
  - ownership and lifetime;
  - allocation strategy;
  - scalability tier.
- **Hot paths:**
  - no hidden allocations;
  - data-oriented layout;
  - justify every virtual dispatch.
- **New subsystems:**
  - go behind an interface with a test seam;
  - land as gated milestones with checks (a readiness work item with acceptance criteria and tests), not as one unverified drop.

## Architecture (key directories)

```
SparkEngine/Source/Core/                 — Platform.h, EngineContext.h
SparkEngine/Source/Camera/               — Camera system
SparkEngine/Source/Graphics/             — GraphicsEngine, Shader, PostProcessing, RHI abstraction layer
SparkEngine/Source/Graphics/RHI/         — Multi-API RHI (D3D11/D3D12/Vulkan/Metal/OpenGL backends)
SparkEngine/Source/Graphics/RenderGraph/ — Render graph system
SparkEngine/Source/Engine/ECS/           — CoreComponents.h + 12 domain component headers, Systems/ECSystems.h
SparkEngine/Source/Engine/AI/            — AISystem, BehaviorTree, NavMesh
SparkEngine/Source/Engine/Animation/     — Skeletal animation, IK, state machines
SparkEngine/Source/Engine/Networking/    — NetworkManager, AreaServer, WorldServer
SparkEngine/Source/Engine/Streaming/     — SeamlessAreaManager, SceneTransitionManager
SparkEngine/Source/Engine/World/         — WorldOriginSystem (origin rebasing)
SparkEngine/Source/Engine/Scripting/     — AngelScript VM, hot-reload, script context
SparkEngine/Source/Engine/2D/            — 2D rendering and sprite systems
SparkEngine/Source/Engine/Cinematic/     — Sequencer, playback
SparkEngine/Source/Engine/Coroutine/     — Async coroutine scheduler
SparkEngine/Source/Engine/Destruction/   — Destructible objects
SparkEngine/Source/Engine/Dialogue/      — Branching dialogue system
SparkEngine/Source/Engine/Events/        — Event bus / event system
SparkEngine/Source/Engine/Gameplay/      — Inventory, quest, achievement, weapon mechanics
SparkEngine/Source/Engine/Loading/       — Loading screens and management
SparkEngine/Source/Engine/Localization/  — Localization system
SparkEngine/Source/Engine/Mobile/        — Mobile platform support
SparkEngine/Source/Engine/Modding/       — Game modding support
SparkEngine/Source/Engine/Replay/        — Record/playback system
SparkEngine/Source/Engine/SaveSystem/    — Save/load persistence
SparkEngine/Source/Engine/UI/            — UI system
SparkEngine/Source/Engine/Tween/          — Tween system with easing functions
SparkEngine/Source/Engine/Persistence/   — Async database-backed persistence
SparkEngine/Source/Engine/Physics/       — Physics-specific engine utilities
SparkEngine/Source/Engine/Editor/        — Engine-side editor utilities
SparkEngine/Source/Engine/VR/            — VR headset/controller/tracking (OpenXR-ready stub, wired in)
SparkEngine/Source/Utils/                — Console, Logger, Profiler, Assert
SparkEditor/Source/Communication/        — CollaborativeEditSession (multi-user editing)
SparkEditor/Source/                      — ImGui editor (22 subsystems, 64 specialized panels)
GameModules/                             — Game module directory (auto-discovered by CMake, 11 modules)
GameModules/SparkGame/Source/            — Base game module (DLL)
GameModules/SparkGameFPS/Source/         — FPS game module (DLL)
GameModules/SparkGameMMO/Source/         — MMO game module (DLL)
GameModules/SparkGameMMOFPS/Source/      — MMO-FPS game module (DLL)
GameModules/SparkGameRPG/Source/         — RPG game module (DLL)
GameModules/SparkGameARPG/Source/        — Action RPG game module (DLL)
GameModules/SparkGameRTS/Source/         — RTS game module (DLL)
GameModules/SparkGameRacing/Source/      — Racing game module (DLL)
GameModules/SparkGamePlatformer/Source/  — Platformer game module (DLL)
GameModules/SparkGameOpenWorld/Source/   — Open-world game module (DLL)
GameModules/SparkGameVisualScript/Source/ — Visual script game module (DLL)
SparkConsole/src/                        — Standalone console application
SparkShaderCompiler/src/                 — Shader compilation tool
SparkSDK/                                — Public SDK/interface headers
FuzzerTests/                             — libFuzzer harnesses, corpora, fuzz policy (separate from Tests/)
Tests/                                   — 7716 test definitions across 652 files, CTest
```

NullRHIDevice automatically activates when no GPU backend is available — engine continues in headless mode. GLAD (OpenGL loader) and SDL2 are bundled in `ThirdParty/`. SDL2 requires `libgl-dev` before CMake configure on Linux.

### ECS execution order

Physics → Animation → AI → Audio → Lifecycle → Render

### Thread safety rules

- `SimpleConsole` — thread-safe (mutex)
- `PhysicsSystem` — Jolt physics; supports multithreaded job dispatch
- `GraphicsEngine` — main thread render, `std::atomic` frame state
- `NetworkManager` — queue mutex for message I/O and handler registration

## Build

```bash
# Generate (pick one)
cmake --preset windows-release       # Windows MSVC
cmake --preset linux-gcc-release     # Linux GCC
cmake --preset macos-release         # macOS Apple Clang (experimental)

# Build and test (each preset writes build/<preset>; the Visual Studio tree needs --config / -C)
cmake --build build/windows-release --config Release && ctest --test-dir build/windows-release -C Release --output-on-failure
cmake --build build/linux-gcc-release && ctest --test-dir build/linux-gcc-release --output-on-failure
```

**Run a subset of `SparkTests`** while iterating. `Tests/TestMain.cpp` reads these environment variables:

```bash
SPARK_TEST_FILE=TestFPSMultiplayer.cpp build/linux-gcc-release/bin/SparkTests   # tests from one source file
SPARK_TEST_NAME=Showcase build/linux-gcc-release/bin/SparkTests                 # name contains ("RPG_" also hits "ARPG_*")
SPARK_TEST_NAME_PREFIX=RPG_ build/linux-gcc-release/bin/SparkTests              # anchored name prefix
SPARK_TEST_EXCLUDE=Soak,Stress build/linux-gcc-release/bin/SparkTests           # comma-separated name substrings to skip
SPARK_TEST_LIMIT=50 build/linux-gcc-release/bin/SparkTests                      # first N tests (bisection)
```

`SPARK_TEST_EXPECT_COUNT=N` fails the run unless exactly N tests were selected; CTest registrations use it to pin a test family. Pass `--warn-is-error` to match how those registrations run.

**Fast local rebuilds:** configure an iterate-and-test tree with `-DENABLE_LTO=OFF`, ccache and mold. `SparkTests` then relinks in seconds instead of re-running LTO over the whole binary; CI Release lanes keep LTO. The exact configure line and the ccache settings the precompiled header needs are in `wiki/development/Workflow-Patterns.md` (Fast Local Rebuilds).

CMake 3.25+, C++23 required. GCC 13+, Clang 17+, or MSVC 19.36+ (VS 2022 17.6+). Key toggles: `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_NETWORKING` (ON by default), `ENABLE_VULKAN`, `ENABLE_OPENGL`, `ENABLE_METAL` (OFF), `ENABLE_DXR`, `ENABLE_HYBRID_RT`, `ENABLE_RECAST`, `ENABLE_SDL2` (auto-ON on Linux), `SPARK_HEADLESS_SUPPORT`, `SPARK_DOUBLE_PRECISION_PHYSICS` (OFF), `BUILD_TESTS`, `BUILD_GAME_MODULES` (ON by default — set OFF for engine-only builds).

**Cross-compilation (MinGW + Wine):** Build Windows D3D11 code on Linux via MinGW, run under Wine + DXVK/Lavapipe. See `wiki/development/MinGW-Wine-Cross-Compilation.md` for full setup. Presets: `linux-mingw-release`, `linux-mingw-debug`.

**Software rendering:** Every RHI backend has a GPU-less fallback — WARP (D3D11/D3D12), Lavapipe (Vulkan), llvmpipe (OpenGL), or NullRHIDevice (headless). See `wiki/advanced/Codebase-Observations.md` for details.

## Git Sync Workflow

Run this before every session start and before every commit/push. The default upstream branch is `Working` (not `main`).

```bash
git fetch origin Working
git log --oneline HEAD..origin/Working | wc -l   # check if behind
git rebase origin/Working                         # if behind, rebase
# If conflicts: resolve, git add <files>, git rebase --continue
```

**Rules:**
- **Never** commit or push while behind the base branch.
- **Rebase only a branch nobody else has pulled.** Once a branch is pushed and shared (an open PR, or several agents or sessions committing to it), bring `Working` in with `git merge origin/Working` instead. Rebasing a shared branch forces a force-push, which breaks every other checkout of it. Never force-push a shared branch.
- After rebasing, re-run `docs/sync-wiki.sh sync` to pick up upstream changes.
- Prefer upstream changes for auto-generated content (`<!-- AUTO:* -->` sections).

## Pre-commit checks

Run checks **appropriate to the files you changed**.

### Docs-only changes (`.md`, `wiki/`, `docs/`, `.claude/`)

```bash
docs/update-all-docs.sh           # One command updates everything
```

Or run individual scripts:

```bash
docs/sync-wiki.sh sync            # Update AUTO: sections in wiki pages
docs/generate-api-docs.sh check   # Regenerate API docs if headers changed
docs/generate-flowchart.sh generate  # Regenerate architecture flowchart
docs/update-codebase-stats.sh generate  # Regenerate Codebase-Statistics.md
docs/update-readme-badges.sh update    # Update README counts & badge JSON
docs/update-context.sh update          # Update CLAUDE.md counts
```

### Code changes (`.h`, `.hpp`, `.cpp`, `CMakeLists.txt`)

```bash
# 1. Format check — CI's check-format roots and incremental scope (wiki/development/Clang-Format.md):
#    every C++ file changed since Working, committed or not. Never truncate the list with head -N.
git diff --name-only --diff-filter=ACMR origin/Working -- \
    SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
    SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src \
    SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests FuzzerTests \
  | grep -E '\.(h|hpp|cpp)$' | grep -v '/Metal/' \
  | xargs -r clang-format --dry-run --Werror

# 2. Fix formatting (if step 1 fails) — same file list
git diff --name-only --diff-filter=ACMR origin/Working -- \
    SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
    SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src \
    SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests FuzzerTests \
  | grep -E '\.(h|hpp|cpp)$' | grep -v '/Metal/' \
  | xargs -r clang-format -i

# 3. CMake configure
cmake --preset linux-gcc-release 2>&1 | tail -20

# 4. Build
cmake --build build/linux-gcc-release 2>&1 | tail -30

# 5. Tests
ctest --test-dir build/linux-gcc-release --output-on-failure

# 6. Docs (one command updates all wikis, stats, badges, context)
docs/update-all-docs.sh
```

If any step fails, fix before committing. CI enforces clang-format on every PR.

```bash
# 7. Validation checks (optional but recommended)
tools/validate-all.sh --warn-only
```

### Documentation scripts reference

| Script | What it updates | Speed |
|--------|----------------|-------|
| `docs/update-all-docs.sh` | Runs all scripts below in order | ~30s |
| `docs/update-all-docs.sh quick` | Skips API docs + flowchart | ~10s |
| `docs/sync-wiki.sh sync` | Wiki AUTO: sections (components, systems, panels, tests) | ~2s |
| `docs/generate-api-docs.sh check` | API reference pages + symbol TSV (`docs/api/.symbols.tsv`) | ~15s |
| `docs/generate-symbol-index.sh generate` | SymbolIndex/FunctionIndex/ClassIndex/EnumIndex/MacroIndex (consumes the TSV) | ~2s |
| `docs/generate-file-tree.sh generate` | `docs/api/FileTree.md` (every source file, LOC, Mermaid module graph) | ~10s |
| `docs/generate-class-hierarchy.sh generate` | `docs/api/ClassHierarchy.md` (Mermaid classDiagram per module) | ~5s |
| `docs/generate-flowchart.sh generate` | Engine-Architecture-Flowchart.md | ~5s |
| `docs/update-codebase-stats.sh generate` | Codebase-Statistics.md (all metrics) | ~5s |
| `docs/update-readme-badges.sh update` | README.md, badge JSON, AI prompts | ~3s |
| `docs/update-context.sh update` | CLAUDE.md counts | ~2s |

All scripts support `check` mode (dry-run, exit 1 if stale).

### Validation scripts reference

| Script | What it checks | Speed |
|--------|---------------|-------|
| `tools/validate-all.sh` | Runs all checks below | ~30s |
| `tools/check-pragma-once.sh` | All headers use `#pragma once` | ~2s |
| `tools/check-editor-panels.sh` | All panels registered in EditorPanelFactory | ~1s |
| `tools/check-wiki-nav.sh` | Wiki pages match `_Sidebar.md` | ~1s |
| `tools/check-wiring.sh` | Systems with Initialize() are called | ~10s |
| `tools/check-bloat.sh` | Files under 500/.cpp 300/.h line thresholds | ~5s |
| `tools/check-doxygen-coverage.sh` | Headers have @file/@brief docs (95% threshold) | ~3s |

## Post-PR checks

After creating or pushing to a PR, **always** poll CI and fix failures before moving on.

```bash
sleep 15
gh pr checks --watch --fail-fast

# If a check fails:
gh run list --branch "$(git branch --show-current)" --limit 5
gh run view <RUN_ID> --log-failed
# Fix locally, commit, push, re-poll
```

Cloud sessions have no `gh` CLI. There, use the GitHub MCP tools instead: `pull_request_read` for PR status and checks, `actions_list` for runs, and `get_job_logs` for failed-job logs.

To reproduce CI failures locally, see `wiki/development/CI-Reproducible-Builds.md` for exact build commands for each job.

### CI jobs summary

| Job | Runner | Compiler | Configs | Key flags |
|-----|--------|----------|---------|-----------|
| `check-format` | ubuntu-24.04 | clang-format | — | `--dry-run --Werror` |
| `validate-prompts` | ubuntu-24.04 | — | — | `--ci` |
| `build-linux-gcc` | ubuntu-24.04 | GCC | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-clang` | ubuntu-24.04 | Clang | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan + LSan |
| `build-linux-tsan` | ubuntu-24.04 | GCC | Debug | TSan (thread races) |
| `build-linux-msan` | ubuntu-24.04 | Clang + MSan-instrumented libc++ 18.1.3 (built in-job, cached) | Debug | MSan + ignorelist, `-DENABLE_VULKAN=OFF`, `continue-on-error` |
| `build-windows-vs2022` | windows-2022 | MSVC v143 | Debug, Release | Ninja Multi-Config + sccache (hash-pinned, `SCCACHE_DIR` restore/save), `-DBUILD_TESTS=ON -DBUILD_GAME_MODULES=ON` |
| `build-windows-vs2026` | windows-2025-vs2026 | MSVC v145 | Debug, Release | Ninja Multi-Config + sccache, `continue-on-error` |
| `build-linux-mingw-wine` | ubuntu-24.04 | MinGW-w64 + Wine | Release | `workflow_dispatch` only, `continue-on-error`, experimental |
| `build-macos` | macos-latest | Apple Clang | Debug, Release | `continue-on-error` |
| `coverage` | ubuntu-24.04 | GCC | Debug | `--coverage` + lcov, per-subsystem thresholds |
| `clang-tidy` | ubuntu-24.04 | Clang | Debug | blocking job; individual diagnostics advisory |
| `todo-count` | ubuntu-24.04 | — | — | fails above 20 (required) |
| `build-windows-shipping` | windows-2022 | MSVC v143 | MinSizeRel | `windows-shipping` preset (Visual Studio generator, no compiler cache), module-profile lifecycle |
| `reproducibility-windows` | windows-2022 | MSVC v143 | MinSizeRel | two `windows-shipping` checkouts built and installed, compared by `tools/compare_build_outputs.py`, `continue-on-error` |

`build-linux-msan`, `build-windows-vs2026`, `build-linux-mingw-wine` (manual `workflow_dispatch` only), `reproducibility-windows` (until a hosted run shows equivalent trees), and `build-macos` are job-level `continue-on-error` — failures are warnings, not blockers. `clang-tidy` is a blocking dependency of `required-ci-gate` (its configure/compile failures block; individual diagnostics are advisory).

Legacy branch protection is not configured on `Working` (`branches/Working/protection` is 404). The repository's `Working integrity` ruleset (21968740) is active, protects against deletion and non-fast-forward updates, and requires the GitHub Actions `Required CI Gate` check with no bypass actors. Re-verify with `python3 .github/scripts/verify-working-ruleset.py --live` (last run 2026-09-24). Exact-SHA evidence and controlled-failure behavior remain release gates tracked as `CI-100`; do not present a green check list alone as release proof.

## Documentation

Six scripts keep all documentation, wikis, badges, and context up to date:

```bash
docs/update-all-docs.sh              # Master script — runs all 6 below in order
docs/update-all-docs.sh quick        # Skip slow steps (API docs, flowchart)
docs/update-all-docs.sh check        # Dry-run — report what's out of date
```

Individual scripts (all support `check` mode):

```bash
docs/sync-wiki.sh sync               # Wiki AUTO: sections (components, systems, panels, tests)
docs/generate-api-docs.sh generate   # API reference (~250 headers → ~240 pages)
docs/generate-flowchart.sh generate  # Engine-Architecture-Flowchart.md
docs/update-codebase-stats.sh generate  # Codebase-Statistics.md (LOC, file counts, largest files)
docs/update-readme-badges.sh update     # README.md counts, badge JSON, AI prompt files
docs/update-context.sh update           # CLAUDE.md counts
python3 tools/publish-wiki.py --check   # Validate the flat GitHub Wiki publication
```

**What gets auto-generated:**
- `docs/api/` — per-header API pages, component/system indices
- `wiki/` AUTO: sections — live component, system, panel, test inventories
- GitHub Wiki — flattened publication of the canonical `wiki/` tree with Gollum-compatible links
- `wiki/getting-started/Engine-Architecture-Flowchart.md` — architecture ASCII diagrams
- `wiki/advanced/Codebase-Statistics.md` — all code metrics
- `.github/badges/*.json` — LOC and file count badges for README
- README.md, CLAUDE.md — hardcoded counts

**Requirements:** Whenever code is added, modified, or deleted:
1. Run `docs/update-all-docs.sh` (included in pre-commit checks step 6)
2. Update the relevant `wiki/` page. New subsystem → new wiki page + add to `wiki/_Sidebar.md`
3. Ensure public headers have Doxygen-style comments (`@brief`, `@param`, `@return`)

Legacy Doxygen is optional: `cd docs && ./generate-docs.sh`

## Readiness Contract

Release readiness is tracked per work item in `docs/readiness/work-items/*.json`. Each criterion carries an `acceptanceStatus` entry, keyed by `criterionDigest` (`criterion_digest()` in `tools/site-data/common.py`):

| State | Required evidence |
|-------|-------------------|
| `unmet` | none |
| `implemented` | at least one repo path (code, test, doc) |
| `evidenced` | a `ci:<workflow>/<run>@<40-hex commit>` reference from an exact-commit CI run |

An item is `done` only when every criterion is `evidenced`; an `open` item records no progress. Local test passes justify `implemented`, never `evidenced`. After editing work items, run `python3 tools/site-data/validate.py` and `python3 tools/site-data/render_handoff.py`, and commit the regenerated `docs/readiness/ENGINE_READINESS_HANDOFF.md` with them.

## Shared Working Tree (Multiple Agents)

When several agents or sessions work in one checkout:

- **Uncommitted edits may belong to a live agent.** Don't commit, revert, stash or reformat files you did not change. Commit only your own hunks, and stage them with `git apply --cached` or `git update-index --cacheinfo`, never with `git add` on a shared file.
- **Serialize the shared build** through `tools/build-lock.sh`, and hold one commit lock (`flock <lockfile>`) around index, commit and push, so nobody else's staging lands in your commit.
- **Never kill or `pkill` a process you did not start.** Pattern-matching `pkill -f` has killed other agents' test runs and deadlocked the build lock.
- **Watch disk.** Keep scratch worktrees and private builds out of the repo and remove them when done. Tree sizes are in `wiki/development/Workflow-Patterns.md`.

## Wiring Things In — Functionality Is Not Optional

A system that exists but is never initialized, called, or connected is **worse than not existing**.

- **Every system must be initialized.** If `Initialize()` exists, it must be called in the startup path.
- **Every update loop must be called.** If `Update()` or `ProcessCommands()` exists, it must appear in the main loop.
- **Every sink must have a source.** If a system receives data, something must be sending it.

Wire systems in with minimal code — call the real function directly, don't wrap it in another abstraction. If you discover a system that is built but not wired in: **either wire it in immediately, or delete it**.

**SparkConsole IPC:** ConsoleProcessManager launches the subprocess and owns the pipe. It must be initialized at engine startup and `ProcessCommands()` called each frame. SimpleConsole is the engine-side log sink only.

## Persistence Context (Wiki)

Project knowledge lives in the **wiki**, the single source of truth for humans and AI sessions. The old `.claude/knowledge` store was retired on 2026-06-08 and its content migrated into the wiki (see `.claude/README.md`). Knowledge pages live under:

- `wiki/development/` — dev workflows, build/CI, git, formatting, cross-compilation, live testing
- `wiki/research/` — engine/library analyses, evaluations, recommendations, external research
- `wiki/advanced/` — codebase audits/observations, system status, architecture notes/decisions

### When to write or update a wiki page

| Trigger | Where |
|---------|-------|
| A problem required multiple attempts to solve | new/updated page under the relevant section |
| A workflow or approach proved consistently effective | `wiki/development/` |
| A faster/better way to do something was discovered | `wiki/development/` |
| A non-obvious codebase/tooling fact was discovered | `wiki/advanced/` |
| An architectural or style decision was made | `wiki/advanced/` |

New pages use `wiki/_Template.md` (Audience / Thread Context / Platform-Backend Scope header + canonical sections, ending with a `## Source & Freshness` note). Add the page to `wiki/_Sidebar.md`, run `docs/sync-wiki.sh sync`, and commit alongside the change.

### At session end

Review whether anything learned warrants a new or updated entry — especially optimizations, patterns, and observations discovered incidentally. Positive learning is equally worth recording.

**Rules:**
- Do not exclude `.claude/` from `.promptignore`
- Always commit context changes — future sessions on any branch benefit
- Prefer updating an existing entry over creating a new one for the same topic

### Asset workflow

Use Blender for asset creation, repair, and export work. Preserve editable source assets and verify exported files through the engine's asset pipeline; a successful Blender export alone is not release qualification.

- **Headless Blender:** `PYTHONHOME=/usr blender -b --factory-startup --python <script> -- <args>`. Add `xvfb-run` for Workbench preview renders.
- **Audio:** the runtime decodes WAV only, so ship music and effects as `.wav`.
- **Module asset references:** `tools/check-module-asset-refs.py` fails closed for modules in its `ENFORCED_MODULES`. Each enforced module lists every asset path its source names, with sha256 and provenance rule, in `GameModules/<Module>/asset-references.json`. Every licensed or authored source has an entry in `tools/asset-integrity/provenance.json`.
