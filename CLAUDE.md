# SparkEngine — Claude Code Context

## What is this?

SparkEngine is a C++20 open-source 3D game engine targeting first-person shooters.
- **Rendering**: DirectX 11 (Windows), OpenGL stubs (Linux/macOS)
- **Physics**: Bullet Physics 3
- **Audio**: XAudio2
- **ECS**: EnTT
- **Scripting**: AngelScript
- **Editor**: Dear ImGui
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

CMake 3.16+, C++20 required. Key toggles: `ENABLE_EDITOR`, `ENABLE_GRAPHICS`, `ENABLE_PHYSX`, `ENABLE_AI`, `ENABLE_ANIMATION`, `ENABLE_NETWORKING` (OFF by default).

## Coding Standards

- **C++20**: `constexpr`, `enum class`, structured bindings, `std::format`, concepts
- **Ownership**: `std::unique_ptr` owning, raw pointers non-owning. No naked `new`/`delete`
- **RAII**: D3D11 via `ComPtr`, all resources released in destructors
- **Const-correctness**: `const` on all non-mutating methods and parameters
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` prefix members, `UPPER_SNAKE` macros
- **Headers**: `#pragma once`, forward-declare where possible
- **Style**: Allman braces, 4-space indent, 120-col limit (see `.clang-format`)
- **Zero warnings**: `/W4` on MSVC, `-Wall -Wextra` on GCC/Clang

## Architecture (key directories)

```
SparkEngine/Source/Core/        — Platform.h, EngineContext.h
SparkEngine/Source/Graphics/    — GraphicsEngine (DX11), Shader, PostProcessing
SparkEngine/Source/Engine/ECS/  — Components.h, Systems/ECSystems.h
SparkEngine/Source/Engine/AI/   — AISystem, BehaviorTree, NavMesh
SparkEngine/Source/Engine/Animation/ — Skeletal animation, IK, state machines
SparkEngine/Source/Engine/Networking/ — NetworkManager (disabled by default)
SparkEngine/Source/Utils/       — Console, Logger, Profiler, Assert
SparkEditor/Source/             — ImGui editor (22 subsystems)
SparkGame/Source/               — Example FPS game module (DLL)
Tests/                          — 35+ unit tests, CTest
```

## ECS execution order

Physics → Animation → AI → Audio → Lifecycle → Render

## Thread safety rules

- `SimpleConsole` — thread-safe (mutex)
- `PhysicsSystem` — main thread only
- `GraphicsEngine` — main thread render, `std::atomic` frame state
- `NetworkManager` — queue mutex for message I/O and handler registration

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

After finishing any code change, **always** run these checks in order:

```bash
# 1. Format check — ensure code matches .clang-format
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src SparkGame/Source \
  -name '*.h' -o -name '*.cpp' | head -50 | xargs clang-format --dry-run --Werror 2>&1

# 2. Fix formatting automatically (if step 1 fails)
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src SparkGame/Source \
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
find SparkEngine/Source SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
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
3. **API docs** (`docs/`): Ensure new or changed public headers have Doxygen-style comments (`@brief`, `@param`, `@return`). The `generate-api-docs.sh` script extracts these automatically. Gap analyses go in `docs/gap-analysis/`.
4. **CLAUDE.md**: If the change affects architecture, build toggles, execution order, thread safety rules, or key directories, update this file to keep it accurate.

Skipping documentation is **not acceptable** — treat docs as part of the deliverable, not an afterthought.

## Things to know

- Use `EngineContext` service locator, not deprecated `g_graphics`/`g_input` globals
- Cross-platform types live in `Core/Platform.h` (DirectXMath stubs on Linux)
- Networking is disabled in default builds (`ENABLE_NETWORKING=OFF`)
- VR/AR, DXR, DLSS/FSR are not implemented
- `.clang-format` enforces Microsoft-based style (Allman braces, 120-col, 4-space indent)
- `.clang-tidy` checks for bugprone, modernize, performance, and readability issues
- Doxygen config lives in `docs/Doxyfile.txt`; wiki pages in `wiki/`
- 35+ unit tests in `Tests/`; always run `ctest` after changes
