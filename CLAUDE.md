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

## Session start (run at the beginning of every session)

**Step 1 — Git sync** (see [Git Sync Workflow](#git-sync-workflow) below for the commands):

Sync your branch with the latest upstream `Working` branch. This is the **first thing** to do — before reading code, before making changes, before anything else. Feature branches diverge as other PRs merge; without rebasing you'll be working on stale code.

**Step 2 — Load persistent context:**

```bash
cat .claude/index.md
```

Scan the index for topics relevant to the current task. Read those knowledge files before proceeding.

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

- **C++23**: `constexpr`, `enum class`, structured bindings, `std::format`, `std::expected`, `std::print`, concepts, deducing `this`, `if consteval`, `std::unreachable`
- **Ownership**: `std::unique_ptr` owning, raw pointers non-owning. No naked `new`/`delete`
- **RAII**: D3D11 via `ComPtr`, all resources released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` prefix members, `UPPER_SNAKE` macros
- **Headers**: `#pragma once`, forward-declare where possible
- **Style**: Allman braces, 4-space indent, 120-col limit (see `.clang-format`)
- **Zero warnings**: `/W4` on MSVC, `-Wall -Wextra` on GCC/Clang
- **Service locator**: Use `EngineContext::Get()->GetX()` for subsystem access. Engine-lifetime ownership lives in the `EngineRuntime` struct (Core-internal; `Core/EngineRuntime.h`) — do not introduce new file-scope `g_*` subsystem globals
- **Cross-platform types**: `Core/Platform.h` (DirectXMath stubs on Linux)

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
SparkEditor/Source/                      — ImGui editor (22 subsystems, 59 specialized panels)
GameModules/                             — Game module directory (auto-discovered by CMake, 10 modules)
GameModules/SparkGame/Source/            — Base game module (DLL)
GameModules/SparkGameFPS/Source/         — FPS game module (DLL)
GameModules/SparkGameMMO/Source/         — MMO game module (DLL)
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
Tests/                                   — 5925 unit tests across 481 files, CTest
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

# Build
cmake --build build --config Release

# Test
cd build && ctest --output-on-failure
```

CMake 3.25+, C++23 required. GCC 13+, Clang 17+, or MSVC 19.36+ (VS 2022 17.6+). Key toggles: `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_NETWORKING` (ON by default), `ENABLE_VULKAN`, `ENABLE_OPENGL`, `ENABLE_METAL` (OFF), `ENABLE_DXR`, `ENABLE_HYBRID_RT`, `ENABLE_RECAST`, `ENABLE_SDL2` (auto-ON on Linux), `SPARK_HEADLESS_SUPPORT`, `SPARK_DOUBLE_PRECISION_PHYSICS` (OFF), `BUILD_TESTS`, `BUILD_GAME_MODULES` (ON by default — set OFF for engine-only builds).

**Cross-compilation (MinGW + Wine):** Build Windows D3D11 code on Linux via MinGW, run under Wine + DXVK/Lavapipe. See `.claude/knowledge/mingw-wine-cross-compilation.md` for full setup. Presets: `linux-mingw-release`, `linux-mingw-debug`.

**Software rendering:** Every RHI backend has a GPU-less fallback — WARP (D3D11/D3D12), Lavapipe (Vulkan), llvmpipe (OpenGL), or NullRHIDevice (headless). See `.claude/knowledge/codebase-observations.md` for details.

## Git Sync Workflow

Run this before every session start and before every commit/push. The default upstream branch is `Working` (not `main`).

```bash
git fetch origin Working
git log --oneline HEAD..origin/Working | wc -l   # check if behind
git rebase origin/Working                         # if behind, rebase
# If conflicts: resolve, git add <files>, git rebase --continue
```

**Rules:**
- **Never** commit or push while behind the base branch. Always rebase first.
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
docs/update-context.sh update          # Update .claude/index.md & CLAUDE.md
```

### Code changes (`.h`, `.hpp`, `.cpp`, `CMakeLists.txt`)

```bash
# 1. Format check
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules \
  -name '*.h' -o -name '*.cpp' | head -50 | xargs clang-format --dry-run --Werror 2>&1

# 2. Fix formatting (if step 1 fails)
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules \
  -name '*.h' -o -name '*.cpp' | xargs clang-format -i

# 3. CMake configure
cmake --preset linux-gcc-release 2>&1 | tail -20

# 4. Build
cmake --build build --config Release 2>&1 | tail -30

# 5. Tests
cd build && ctest --output-on-failure && cd ..

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
| `docs/update-context.sh update` | .claude/index.md, CLAUDE.md | ~2s |

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

To reproduce CI failures locally, see `.claude/knowledge/ci-reproducible-builds.md` for exact build commands for each job.

### CI jobs summary

| Job | Runner | Compiler | Configs | Key flags |
|-----|--------|----------|---------|-----------|
| `check-format` | ubuntu-24.04 | clang-format | — | `--dry-run --Werror` |
| `validate-prompts` | ubuntu-24.04 | — | — | `--ci` |
| `build-linux-gcc` | ubuntu-24.04 | GCC | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-clang` | ubuntu-24.04 | Clang | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan + LSan |
| `build-linux-tsan` | ubuntu-24.04 | GCC | Debug | TSan (thread races) |
| `build-linux-msan` | ubuntu-24.04 | Clang + libc++ | Debug | MSan + ignorelist, `continue-on-error` |
| `build-windows-vs2022` | windows-latest | MSVC v143 | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-windows-vs2026` | windows-latest | MSVC v144 | Debug, Release | `continue-on-error` |
| `build-linux-mingw-wine` | ubuntu-24.04 | MinGW-w64 + Wine | Release | `continue-on-error` |
| `build-macos` | macos-latest | Apple Clang | Debug, Release | `continue-on-error` |
| `coverage` | ubuntu-24.04 | GCC | Debug | `--coverage` + lcov, per-subsystem thresholds |
| `clang-tidy` | ubuntu-24.04 | Clang | Debug | `continue-on-error` |
| `todo-count` | ubuntu-24.04 | — | — | threshold: 20 |

`build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, and `clang-tidy` use `continue-on-error` — failures are warnings, not blockers.

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
docs/update-context.sh update           # .claude/index.md and CLAUDE.md counts
```

**What gets auto-generated:**
- `docs/api/` — per-header API pages, component/system indices
- `wiki/` AUTO: sections — live component, system, panel, test inventories
- `wiki/getting-started/Engine-Architecture-Flowchart.md` — architecture ASCII diagrams
- `wiki/advanced/Codebase-Statistics.md` — all code metrics
- `.github/badges/*.json` — LOC and file count badges for README
- README.md, CLAUDE.md, .claude/index.md — hardcoded counts

**Requirements:** Whenever code is added, modified, or deleted:
1. Run `docs/update-all-docs.sh` (included in pre-commit checks step 6)
2. Update the relevant `wiki/` page. New subsystem → new wiki page + add to `wiki/_Sidebar.md`
3. Ensure public headers have Doxygen-style comments (`@brief`, `@param`, `@return`)

Legacy Doxygen is optional: `cd docs && ./generate-docs.sh`

## Wiring Things In — Functionality Is Not Optional

A system that exists but is never initialized, called, or connected is **worse than not existing**.

- **Every system must be initialized.** If `Initialize()` exists, it must be called in the startup path.
- **Every update loop must be called.** If `Update()` or `ProcessCommands()` exists, it must appear in the main loop.
- **Every sink must have a source.** If a system receives data, something must be sending it.

Wire systems in with minimal code — call the real function directly, don't wrap it in another abstraction. If you discover a system that is built but not wired in: **either wire it in immediately, or delete it**.

**SparkConsole IPC:** ConsoleProcessManager launches the subprocess and owns the pipe. It must be initialized at engine startup and `ProcessCommands()` called each frame. SimpleConsole is the engine-side log sink only.

## Persistence Context Database

The `.claude/` directory is persistent AI memory — a knowledge base that Claude reads and writes across sessions. It captures issue fixes, effective workflows, optimizations, codebase observations, and project decisions. See `.claude/README.md` for entry format, rules, and directory structure.

### When to write a new entry

| Trigger | Entry type |
|---------|-----------|
| A problem required multiple attempts to solve | **Issue** |
| A workflow or approach proved consistently effective | **Pattern** |
| A faster/better way to do something was discovered | **Optimization** |
| A non-obvious codebase/tooling fact was discovered | **Observation** |
| An architectural or style decision was made | **Decision** |

After writing, update `.claude/index.md` and commit both alongside code changes.

### At session end

Review whether anything learned warrants a new or updated entry — especially optimizations, patterns, and observations discovered incidentally. Positive learning is equally worth recording.

**Rules:**
- Do not exclude `.claude/` from `.promptignore`
- Always commit context changes — future sessions on any branch benefit
- Prefer updating an existing entry over creating a new one for the same topic
