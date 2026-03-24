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
- **Primary platform**: Windows 10+ (MSVC); Linux/macOS are experimental

## Build

```bash
# Generate (pick one)
cmake --preset windows-release       # Windows MSVC
cmake --preset linux-gcc-release     # Linux GCC

# Build
cmake --build build --config Release

# Test
cd build && ctest --output-on-failure
```

CMake 3.25+, C++23 required. GCC 13+, Clang 17+, or MSVC 19.36+ (VS 2022 17.6+). Key toggles: `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, `ENABLE_NETWORKING` (ON by default), `ENABLE_VULKAN`, `ENABLE_OPENGL`, `ENABLE_SAVE_SYSTEM`, `ENABLE_PROCEDURAL`, `ENABLE_CINEMATIC`, `ENABLE_EVENT_SYSTEM`, `ENABLE_DECALS`, `ENABLE_MESH_LOD`, `ENABLE_DXR` (OFF by default), `BUILD_TESTS`, `BUILD_GAME_MODULES` (ON by default — set OFF for engine-only builds).

## Anti-Bloat Guidelines

These guidelines exist because AI-assisted development has a structural bias toward complexity. Without active awareness, sessions tend to add more than they remove. The goal is **sanity, not sacrifice** — keep code clean and intentional without stripping away legitimate verbosity, comments, or readability.

### The Core Problem

AI defaults to:
- Adding features "just in case" → dead code accumulates
- Creating helper classes for single uses → pointless abstraction
- Over-engineering simple problems → massive files that nobody can debug
- Building systems without wiring them in → ConsoleProcessManager-style orphans
- Scattering related logic → 25 command registration functions instead of 1

### Sensible Thresholds (Not Hard Limits)

These are **guidelines for when to pause and think**, not absolute rules. A 450-line `.cpp` that is clean, well-commented, and logically cohesive is fine. A 200-line `.cpp` full of cryptic compressed code is not. Use judgment.

| Thing | Threshold | What to do |
|-------|-----------|------------|
| `.cpp` file size | ~500 lines | Ask: "Is this one cohesive thing, or two things jammed together?" Split if it's doing multiple jobs. Leave it if it's one coherent unit. |
| `.h` file size | ~300 lines | Ask: "Are these types/declarations related?" Data-heavy headers with many structs are fine. A class header with 40 methods probably needs splitting. |
| Public methods per class | ~15 | Ask: "Does each method earn its place?" If yes, keep them. |
| Function length | ~60 lines | Ask: "Can I understand this at a glance?" Long functions with clear linear flow are fine. Long functions with nested branching should be split. |
| Command registration functions | 1 per subsystem | Consolidate before adding commands |
| Parallel singleton systems doing the same thing | 0 | Remove the duplicate |

### The Readability Principle

**Never sacrifice readability to hit a line count.** Specifically:
- **Comments that explain "why"** are valuable — keep them. Don't strip comments to save lines.
- **Descriptive variable names** are better than terse ones. `brushRadius` > `br`.
- **Vertical whitespace** between logical sections aids scanning. Don't collapse everything.
- **Explicit loop bodies with braces** are clearer than braceless single-line forms when the body is non-trivial.
- **One statement per line** — don't pack `float h = 0.0f, freq = frequency, amp = amplitude;` to save two lines.

The question is always: **"Does this make sense to someone reading it for the first time?"**

### Before Writing Any Code — Ask These Questions

1. **Does this already exist?** Search before writing. If yes, use the existing one.
2. **Will this be called?** If you can't name the caller right now, don't write it.
3. **Can the existing code do this with a small change?** Prefer editing over adding.
4. **Is this a one-time use?** If yes, inline it — no helper function, no new class.
5. **Am I future-proofing?** Stop. Write only what is needed today.

### Before Adding New Files or Classes

- Adding a new class → ask if an existing one can be extended instead
- Adding a new `.cpp` file → ask if it can logically live in an existing file
- Adding a new public method → check if a private or existing method covers the need
- Adding new command registrations → they go in ONE place per subsystem, not scattered

If the answer is genuinely "no, this needs its own thing" — go ahead and add it. New files for clear responsibilities are good architecture, not bloat.

### Dead Code Is Actively Harmful

- Unused public methods → delete, don't comment out
- Uninitialized systems (built but never called) → either wire them in or delete them
- Features built but not integrated → count as bugs, not WIP
- Commented-out code → delete it, git history exists for a reason
- "Stub" implementations → either implement fully or remove entirely

### Signs of Actual Bloat — Pause and Reconsider

- A class has more `Register*` methods than actual logic
- You're adding a 6th logging method when 3 exist
- A new `*Manager` or `*System` class when the existing one can be extended
- Duplicating member variables that exist in a related class
- Creating an abstraction for something used in exactly one place
- A file is growing because unrelated concerns are being added to it

### On Removal

When refactoring, aim to remove dead weight — but removal is a tool, not a mandate. A PR that adds 200 well-structured lines and removes 0 is fine if those 200 lines are genuinely needed. A PR that adds 50 lines of speculative code and removes 0 is bloat. The distinction is intent and necessity, not arithmetic.

---

## Coding Standards

- **C++23**: `constexpr`, `enum class`, structured bindings, `std::format`, `std::expected`, `std::print`, concepts, deducing `this`, `if consteval`, `std::unreachable`
- **Ownership**: `std::unique_ptr` owning, raw pointers non-owning. No naked `new`/`delete`
- **RAII**: D3D11 via `ComPtr`, all resources released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` prefix members, `UPPER_SNAKE` macros
- **Headers**: `#pragma once`, forward-declare where possible
- **Style**: Allman braces, 4-space indent, 120-col limit (see `.clang-format`)
- **Zero warnings**: `/W4` on MSVC, `-Wall -Wextra` on GCC/Clang

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
SparkEngine/Source/Engine/Procedural/    — (planned) Procedural generation — ENABLE_PROCEDURAL toggle exists but no implementation yet
SparkEngine/Source/Engine/Replay/        — Record/playback system
SparkEngine/Source/Engine/SaveSystem/    — Save/load persistence
SparkEngine/Source/Engine/Stats/         — (planned) Performance telemetry — not yet implemented
SparkEngine/Source/Engine/UI/            — UI system
SparkEngine/Source/Engine/VR/            — VR headset/controller/tracking
SparkEngine/Source/Utils/                — Console, Logger, Profiler, Assert
SparkEditor/Source/Communication/        — CollaborativeEditSession (multi-user editing)
SparkEditor/Source/                      — ImGui editor (22 subsystems, 32 specialized panels)
GameModules/                             — Game module directory (auto-discovered by CMake)
GameModules/SparkGame/Source/            — Example FPS game module (DLL)
GameModules/SparkGameMMO/Source/         — Example MMO game module (DLL)
SparkConsole/src/                        — Standalone console application
SparkShaderCompiler/src/                 — Shader compilation tool
SparkSDK/                                — Public SDK/interface headers
Tests/                                   — 145 unit tests, CTest
```

## ECS execution order

Physics → Animation → AI → Audio → Lifecycle → Render

## Thread safety rules

- `SimpleConsole` — thread-safe (mutex)
- `PhysicsSystem` — Jolt physics; supports multithreaded job dispatch
- `GraphicsEngine` — main thread render, `std::atomic` frame state
- `NetworkManager` — queue mutex for message I/O and handler registration

## Session start (run at the beginning of every session)

At the start of every new session, **immediately** sync your branch with the latest upstream `Working` branch before doing anything else:

```bash
# 1. Fetch the latest upstream commits
git fetch origin Working

# 2. Check how far behind you are
git log --oneline HEAD..origin/Working | wc -l

# 3. If behind, rebase onto the latest
git rebase origin/Working

# 4. If conflicts arise, resolve them (prefer upstream for auto-generated content)
git add <resolved-files>
git rebase --continue
```

**Why:** Feature branches diverge from `Working` as other PRs merge. Without rebasing at session start, you'll be working on stale code and face larger conflicts later.

**Rules:**
- This is the **first thing** to do in every session — before reading code, before making changes, before anything else.
- If the rebase produces conflicts, resolve them carefully and re-run `docs/sync-wiki.sh sync`.
- The default upstream branch is `Working` (not `main`).

After the git sync, **read `.claude/index.md`** to load session context (step 5 below).

```bash
# 5. Load persistent context
cat .claude/index.md

# 6. Bloat check — identify the worst files before touching anything
find SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
  -name '*.cpp' | xargs wc -l | sort -rn | head -15
```

Step 6 takes 2 seconds and tells you immediately what is over the line-count limit. If the task involves any of those files, the first job is to trim them, not add to them.

## Persistence Context Database

The `.claude/` directory is a persistent AI memory store — a general self-learning knowledge base that Claude reads and writes across sessions. It captures everything worth remembering: issue fixes, effective workflows, optimizations, codebase observations, and project decisions. Future sessions start with this accumulated knowledge rather than from scratch.

### At session start

After the git sync (steps 1–4 above), read the index:

```bash
cat .claude/index.md
```

Scan the index for topics relevant to the current task or domain. Read those files before proceeding.

### When to write a new entry

Write or update a `.claude/knowledge/` file whenever Claude learns something worth preserving:

| Trigger | Entry type |
|---------|-----------|
| A problem required multiple attempts to solve | **Issue** |
| A workflow or approach proved consistently effective | **Pattern** |
| A faster/better way to do something was discovered | **Optimization** |
| A non-obvious codebase/tooling fact was discovered | **Observation** |
| An architectural or style decision was made | **Decision** |

After writing, update `.claude/index.md` and commit both alongside code changes.

### Entry format

```markdown
# [Topic]

**Last updated:** YYYY-MM-DD
**Type:** Issue | Pattern | Optimization | Observation | Decision
**Status:** Resolved | Active | Ongoing | Superseded

## Description
## Context
## Methods Tried  ← Issue only; numbered, each ending with → FAILED / → WORKED
## Approach       ← Pattern/Optimization only
## Details        ← Observation/Decision only
## Solution / Summary
## Notes
```

### Structure

```
.claude/
├── README.md                              # Full usage guide
├── index.md                               # Master index — read at session start
└── knowledge/
    ├── github-api-pr-checks.md            # [Issue] PR check status access
    ├── ci-failures.md                     # [Issue] CI job blocking rules, reproduction
    ├── git-rebase-conflicts.md            # [Issue] Rebase conflict resolution
    ├── clang-format.md                    # [Issue] Full scan required; Metal excluded
    ├── cmake-linux-build-failures.md      # [Issue] Linux CMake configure/build failures
    ├── windows-msvc-w4-warnings.md        # [Issue] MSVC /W4 fix table
    ├── workflow-patterns.md               # [Pattern] Effective dev workflows
    ├── codebase-observations.md           # [Observation] Non-obvious SparkEngine facts
    └── build-optimizations.md            # [Optimization] Build and CI workflow speedups
```

### At session end

After completing any non-trivial task, review whether anything learned during the session warrants a new or updated entry. This is especially important for:
- **Optimizations** discovered incidentally (a faster flag, a better command sequence)
- **Patterns** that worked well and should be repeated
- **Observations** about the codebase that weren't obvious before

Don't wait for something to break. Positive learning — things that worked well — is equally worth recording.

**Rules:**
- Do not exclude `.claude/` from `.promptignore` — Claude must be able to read it.
- Entries are written by Claude sessions; humans may correct factual errors only.
- Always commit context changes — future sessions on any branch benefit.
- Prefer updating an existing entry over creating a new one for the same topic.

## Branch freshness (run before every commit)

Before committing or pushing, **always** ensure your branch is up to date with the upstream base branch:

```bash
# 1. Fetch the latest upstream commits
git fetch origin Working

# 2. Check how far behind you are
git log --oneline HEAD..origin/Working | wc -l

# 3. If behind, rebase onto the latest
git rebase origin/Working

# 4. Resolve any conflicts, then continue
git add <resolved-files>
git rebase --continue
```

**Rules:**
- **Never** commit or push to a branch that is behind the base branch. Always rebase first.
- After rebasing, re-run the doc sync scripts (`docs/sync-wiki.sh sync`) to pick up any upstream changes to auto-generated sections.
- If the rebase produces conflicts, resolve them carefully — prefer upstream changes for auto-generated content (e.g., `<!-- AUTO:* -->` sections in wiki pages).
- The default upstream branch is `Working` (not `main`).

## Pre-commit checks (run before every commit)

After finishing any code change, run the checks **appropriate to the files you changed**. Not every step applies to every commit.

### Docs-only changes (`.md`, `wiki/`, `docs/`, `.claude/`, `README.md`, `CLAUDE.md`)

If you **only** changed markdown, wiki, documentation, or `.claude/` knowledge files — and touched **no** `.h`, `.hpp`, or `.cpp` files — skip steps 1–5 and only run step 6:

```bash
# 6. Update documentation — regenerate API docs and sync wiki
docs/generate-api-docs.sh check
docs/sync-wiki.sh sync
```

### Code changes (`.h`, `.hpp`, `.cpp`, `CMakeLists.txt`, etc.)

If you changed any C++ source, headers, or build files, run **all** steps in order:

```bash
# 1. Format check — ensure code matches .clang-format
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules \
  -name '*.h' -o -name '*.cpp' | head -50 | xargs clang-format --dry-run --Werror 2>&1

# 2. Fix formatting automatically (if step 1 fails)
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules \
  -name '*.h' -o -name '*.cpp' | xargs clang-format -i

# 3. Sanity check — CMake configure (Linux)
cmake --preset linux-gcc-release 2>&1 | tail -20

# 4. Compile — build and verify zero errors
cmake --build build --config Release 2>&1 | tail -30

# 5. Tests — run the test suite
cd build && ctest --output-on-failure && cd ..

# 6. Update documentation — regenerate API docs and sync wiki
docs/generate-api-docs.sh check
docs/sync-wiki.sh sync
```

If any step fails, fix the issue before committing. CI enforces clang-format on every PR.

## Post-PR checks (poll after every PR submission)

After creating or pushing to a pull request, **always** poll the CI checks and fix any failures before moving on. This is mandatory — do not consider a PR complete until all checks pass.

### Polling procedure

```bash
# 1. Wait 15 seconds for checks to start, then poll status
sleep 15
gh pr checks --watch --fail-fast

# 2. If the above is not available or times out, poll manually:
#    Repeat every 30 seconds until all checks complete (up to 15 minutes)
gh pr checks
```

If any check fails:

```bash
# 3. Get full failure logs for the failed job
gh run list --branch "$(git branch --show-current)" --limit 5
gh run view <RUN_ID> --log-failed

# 4. Fix the issue locally, commit, and push
# 5. Re-poll checks until all pass (repeat steps 1-4)
```

### Matching CI build configurations locally

The GitHub Actions workflow (`.github/workflows/build.yml`) runs these jobs. To reproduce failures locally, mirror the exact CI settings:

**clang-format check** (runs on every PR):
```bash
find SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1
# Fix: pipe the same file list to clang-format -i
```

**Linux GCC build** (Debug + Release):
```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**Linux Clang build** (Debug + Release):
```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**Linux GCC AddressSanitizer** (Debug):
```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**Windows MSVC VS 2022 (v143)** (Debug + Release):
```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSPARK_MSVC_TOOLSET=v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

**Prompt validation** (runs on every PR):
```bash
./tools/validate-prompts.sh --ci
```

### CI jobs summary

| Job | Runner | Compiler | Configs | Key flags |
|-----|--------|----------|---------|-----------|
| `check-format` | ubuntu-24.04 | clang-format | — | `--dry-run --Werror` |
| `validate-prompts` | ubuntu-24.04 | — | — | `--ci` |
| `build-linux-gcc` | ubuntu-24.04 | GCC | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-clang` | ubuntu-24.04 | Clang | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan |
| `build-windows-vs2022` | windows-latest | MSVC v143 | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-windows-vs2026` | windows-latest | MSVC v144 | Debug, Release | `continue-on-error` |
| `coverage` | ubuntu-24.04 | GCC | Debug | `--coverage` + lcov |
| `clang-tidy` | ubuntu-24.04 | Clang | Debug | `continue-on-error` |
| `todo-count` | ubuntu-24.04 | — | — | threshold: 20 |

### Rules

- **Never** consider a PR done until `gh pr checks` shows all required checks passing.
- If a check fails, download the failed run logs with `gh run view <ID> --log-failed`, diagnose, fix locally, push, and re-poll.
- For Windows-only failures that cannot be reproduced on Linux, inspect the CI logs carefully and fix based on MSVC-specific diagnostics (e.g., `/W4` warnings, MSVC type conversion rules, Windows SDK headers).
- The `build-windows-vs2026` and `clang-tidy` jobs use `continue-on-error` — failures there are warnings, not blockers.

## Documentation generation

Two custom scripts generate documentation without requiring Doxygen or Graphviz:

```bash
# 1. Generate markdown API reference from all headers (outputs to docs/api/)
docs/generate-api-docs.sh generate    # Full generation (~250 headers → ~240 pages)
docs/generate-api-docs.sh check       # Only regenerate if headers changed (checksum-based)
docs/generate-api-docs.sh status      # Show generation status

# 2. Sync wiki pages with current codebase inventory
docs/sync-wiki.sh sync               # Update auto-generated sections in wiki pages
docs/sync-wiki.sh check              # Dry-run: report what's out of date (exits 1 if stale)
docs/sync-wiki.sh status             # Show codebase + wiki statistics
```

**What gets generated:**
- `docs/api/README.md` — API index grouped by module
- `docs/api/ComponentIndex.md` — All ECS components with source locations
- `docs/api/SystemIndex.md` — All ECS systems with source locations
- `docs/api/SparkEngine/...` — Per-header API pages (classes, methods, enums, members)
- Wiki auto-sections (`<!-- AUTO:name -->` markers) in: `Entity-Component-System.md`, `Testing.md`, `SparkEditor.md`, `Home.md`

**Legacy Doxygen (optional, requires doxygen + graphviz):**
```bash
cd docs && ./generate-docs.sh         # Full Doxygen HTML output
cd docs && ./auto-update.sh check     # Auto-regenerate on header changes
```

## Documentation requirements

Whenever code is **added**, **modified**, or **deleted**, update the corresponding documentation:

1. **Run the doc scripts** — After any code change, run both:
   ```bash
   docs/generate-api-docs.sh check    # Regenerate API pages if headers changed
   docs/sync-wiki.sh sync             # Update wiki inventories (components, systems, panels, tests)
   ```
2. **Wiki pages** (`wiki/`): Update the relevant wiki page for the subsystem affected. If a new subsystem is introduced, create a new wiki page and add it to `wiki/_Sidebar.md`. Existing pages cover: Architecture, ECS, Rendering, Physics, AI, Animation, Audio, Networking, Scripting, Editor, Input, Scene Management, Terrain, Gameplay Systems, Event System, Save System, Shader Pipeline, Asset Pipeline, Day-Night/Weather, Cinematic Sequencer, Testing, Build System, and more.
3. **API docs** (`docs/`): Ensure new or changed public headers have Doxygen-style comments (`@brief`, `@param`, `@return`). The `generate-api-docs.sh` script extracts these automatically. System maturity tracking lives in the `wiki/Codebase-Health.md` wiki page.
4. **CLAUDE.md**: If the change affects architecture, build toggles, execution order, thread safety rules, or key directories, update this file to keep it accurate.

Skipping documentation is **not acceptable** — treat docs as part of the deliverable, not an afterthought.

## Wiring Things In — Functionality Is Not Optional

A system that exists but is never initialized, called, or connected is **worse than not existing**. It adds confusion, maintenance burden, and false confidence. The rule is simple:

- **Every system must be initialized.** If `Initialize()` exists, it must be called somewhere in the startup path.
- **Every update loop must be called.** If `Update()` or `ProcessCommands()` exists, it must appear in the main loop.
- **Every sink must have a source.** If a system receives data (commands, logs, events), something must be sending it.

When wiring a system in, use the absolute minimum code:
```cpp
// CORRECT — minimal, direct, explicit
ConsoleProcessManager::GetInstance().Initialize();  // in startup
ConsoleProcessManager::GetInstance().ProcessCommands();  // in main loop

// WRONG — wrapping in another layer of abstraction before it even works
class ConsoleInitHelper { ... };  // NO. Just call Initialize().
```

If you discover a system that is built but not wired in: **either wire it in immediately, or delete it**. It cannot stay in a half-built state.

## Known Debloat Targets (address these before adding anything new)

These are confirmed bloat problems discovered during audit. They must be fixed before new features are added in the relevant areas.

| File | Problem | Status |
|------|---------|--------|
| `SparkEngine/Source/Utils/SparkConsole.cpp` | Was 261KB with embedded console UI | **Resolved** — refactored to 551 lines |
| `SparkEngine/Source/Utils/ConsoleProcessManager` | Was initialized multiple times | **Resolved** — consolidated to single `InitConsole()` call |
| `SparkEngine/Source/Core/SparkEngine.cpp` | `SimpleConsole::Initialize()` called 3 times | **Resolved** — all paths now use `InitConsole()` |
| `SparkEngine/Source/Physics/PhysicsBodyImpl.cpp` | Used `extern g_physicsSystem` global alias | **Resolved** — migrated to `EngineContext::Get()->GetPhysics()` |
| RHI backends (D3D11/D3D12/Vulkan/OpenGL) | Factory methods returned raw `new` pointers | **Resolved** — migrated to `std::unique_ptr` returns |

## Things to know

- Use `EngineContext` service locator, not deprecated `g_graphics`/`g_input` globals
- Cross-platform types live in `Core/Platform.h` (DirectXMath stubs on Linux)
- Networking is enabled in default builds (`ENABLE_NETWORKING=ON`)
- VR framework exists (`SparkEngine/Source/Engine/VR/`) — OpenXR-ready stub, wired into engine init/update loop. Requires OpenXR SDK for actual VR hardware. DXR raytracing is optional (`ENABLE_DXR=OFF` by default); DLSS/FSR are not implemented
- `.clang-format` enforces Microsoft-based style (Allman braces, 120-col, 4-space indent)
- `.clang-tidy` checks for bugprone, modernize, performance, and readability issues
- Doxygen config lives in `docs/Doxyfile.txt`; wiki pages in `wiki/`
- 82+ unit tests in `Tests/`; always run `ctest` after changes
- **SparkConsole communicates with the engine via stdin/stdout pipes.** ConsoleProcessManager launches the subprocess and owns the pipe. SimpleConsole is the engine-side log sink only — it is not an IPC layer.
- **ConsoleProcessManager must be initialized at engine startup** and `ProcessCommands()` must be called each frame. Without this, SparkConsole.exe never launches and commands are never executed.
