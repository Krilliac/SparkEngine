# Effective SparkEngine Development Workflows

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (commands shown for Linux/bash; see notes for Windows/macOS equivalents)

## Overview

Recurring workflows that have proven consistently effective for SparkEngine development tasks. These patterns reduce time spent on overhead and increase the reliability of outputs. Apply them proactively rather than waiting until something goes wrong.

They apply to all SparkEngine development sessions. Most correspond to gaps where the "obvious" approach is slower or less reliable than the pattern described here.

## Codebase Exploration — Parallel Explore Agents

When a task touches multiple areas of the codebase (e.g., adding a feature that spans ECS + Graphics + Editor), launch 2-3 Explore agents **in parallel** rather than sequentially. Each agent gets a specific search focus.

```
Agent 1: "Search for existing implementations of X in SparkEngine/Source/Engine/ECS/"
Agent 2: "Find all usages of Y in SparkEditor/Source/"
Agent 3: "Identify patterns for Z in Tests/"
```

Do not use a single Explore agent for a broad multi-area search — it will either miss areas or spend too long on one area.

**When to apply:**

- Task involves code in 2+ directories
- Scope is uncertain and you need to map the codebase before planning
- Looking for an existing implementation before writing new code (always check first)

**Notes:**

- Use read-only exploration agents for codebase research; reserve planning agents for architectural design work after exploration.
- 3 agents maximum per parallel batch; quality over quantity.

## Documentation Sync After Structural Changes

After any change that adds, renames, or removes public headers, ECS components, systems, editor panels, or tests — run the doc scripts before committing. The fastest reliable option is the master script:

```bash
docs/update-all-docs.sh           # runs all six doc scripts in order (~30s)
docs/update-all-docs.sh quick     # skips API docs + flowchart (~10s)
```

Or run only the two most commonly needed:

```bash
docs/generate-api-docs.sh check   # regenerates API pages if headers changed
docs/sync-wiki.sh sync            # updates wiki AUTO: sections (component/system/test counts)
```

Then `git add` any changed files under `docs/api/` or `wiki/` alongside the code change.

**When to apply:**

- Added or removed a `.h` file
- Added or removed an ECS component or system
- Added or removed an editor panel
- Added or removed a test

**Notes:**

- The `check-format` CI job will **not** catch stale docs — it only checks C++ formatting. Stale docs surface as unexpected diffs in later sessions.
- `docs/generate-api-docs.sh check` uses checksums — it is fast when nothing changed.
- `docs/sync-wiki.sh sync` updates `<!-- AUTO:* -->` markers in wiki files. Never edit those markers manually.

## Pre-Push Checklist Order

Run pre-push checks in this specific order — each step catches a different error class, and the order minimizes wasted time:

```bash
# 1. Format first (fastest, most common failure)
find SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1

# 2. CMake configure (catches missing headers, broken includes)
cmake --preset linux-gcc-release 2>&1 | tail -20

# 3. Build (catches compile errors)
cmake --build build --config Release --parallel $(nproc) 2>&1 | tail -30

# 4. Tests (catches regressions)
cd build && ctest --output-on-failure && cd ..

# 5. Docs (catches stale auto-generated content)
docs/update-all-docs.sh
```

Stop at the first failure; fix it before proceeding to the next step.

**Notes:**

- Step 1 (format) is cheap (~5 seconds). Do it even for tiny changes — MSVC code often reformats differently.
- Step 2 (configure) detects include-path issues before a full compile.
- Step 5 (docs) is only needed when step 1 touches headers or structural files.
- See [Build Optimizations](Build-Optimizations.md) for the `--parallel $(nproc)` flag in step 3.

## CMake Preset Over Manual Flags

Always prefer a named preset over manually constructing `-B build -DCMAKE_BUILD_TYPE=...` flags for local development.

```bash
# Preferred
cmake --preset linux-gcc-release

# Only use manual flags when a custom toggle is needed
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DENABLE_NETWORKING=ON \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
```

**Notes:**

- Presets are defined in `CMakePresets.json` and are authoritative.
- Available presets include: `windows-debug`, `windows-release`, `windows-shipping`, `windows-development`, `linux-gcc-debug`, `linux-gcc-release`, `linux-clang-debug`, `linux-clang-release`, `linux-shipping`, `linux-development`, `linux-mingw-release`, `linux-mingw-debug`, `macos-debug`, `macos-release`, `macos-metal`, `macos-moltenvk`, plus the CI-specific `ci-linux-asan` and `ci-linux-tsan`.
- Note: the sanitizer/GCC/Clang CI jobs use manual `cmake -B build` flag invocations rather than the matching preset, so to reproduce those jobs exactly use the commands in [CI Reproducible Builds](CI-Reproducible-Builds.md), not the preset.
- If a preset produces unexpected behavior, check `CMakePresets.json` for the exact flags it sets rather than guessing.
- Delete `build/` if switching between preset and manual configure — the cache will conflict.

## Removal Before Addition

Before writing any new code in a file, check its size. SparkEngine's anti-bloat thresholds are ~500 lines for `.cpp` and ~300 for `.h` (guidelines for when to pause, not hard limits — see `CLAUDE.md`). If a file is over the threshold and doing multiple jobs, trim it first:

```bash
# Check size of a file before editing
wc -l SparkEngine/Source/Utils/SparkConsole.cpp

# If over threshold:
# 1. Find dead methods (no callers outside the class)
grep -n "void SimpleConsole::" SparkEngine/Source/Utils/SparkConsole.cpp | head -30
# 2. Delete them
# 3. Then make your actual change
```

For any new public method being added:

1. Search for existing methods that do something similar.
2. If one exists, extend it — don't add a new one.
3. If there are now 2 similar methods after your change, remove the older one.

For any new system/class being added:

1. Search for an existing class that could be extended.
2. If adding, remove something of equivalent complexity.

**When to apply:**

- Every time you open a file to edit it
- Before every PR — check net line delta: should be <= 0 for refactors, minimal positive for features
- When a file starts feeling hard to navigate

**Notes:**

- Removal is not "going backwards" — it is the primary maintenance activity.
- Delete dead code; don't comment it out. Git history exists.
- `tools/check-bloat.sh` enforces the size thresholds and runs as part of `tools/validate-all.sh`.

## Session Start Checklist Order

Every session should start in this exact order before doing anything else:

1. `git fetch origin Working && git log --oneline HEAD..origin/Working | wc -l` — assess how far behind.
2. `git rebase origin/Working` — sync with upstream.
3. Resolve any rebase conflicts (see [Git Rebase Conflicts](Git-Rebase-Conflicts.md)).
4. `cat .claude/index.md` — load persistent context.
5. Read any knowledge files relevant to the current task.
6. **Bloat check** — run before touching anything:
   ```bash
   find SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
     -name '*.cpp' | xargs wc -l | sort -rn | head -15
   ```
7. **Then** start reading code or planning.

Skipping steps 4-5 means starting each session without accumulated knowledge. Skipping step 6 means walking into a bloated file blind.

**Notes:**

- If `wc -l` output is 0 in step 1, the branch is up to date — skip the rebase.
- Never start reading or editing code before completing the git sync. Stale code leads to conflicts on push.
- The bloat check takes ~2 seconds.

## Source & Freshness

- Original entry: `Effective SparkEngine Development Workflows`, last updated 2026-03-14.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - Doc-sync section now leads with `docs/update-all-docs.sh` (the master script), which is the current recommended one-shot; the two-script combo is kept as a faster subset.
  - Pre-push step 5 changed to `docs/update-all-docs.sh` to match current `CLAUDE.md` pre-commit guidance (was two separate scripts).
  - Anti-bloat thresholds updated to the current ~500 `.cpp` / ~300 `.h` guideline values (source quoted older 400/200 figures); noted `tools/check-bloat.sh` as the enforcer.
  - CMake preset list refreshed against `CMakePresets.json`; added new `ci-linux-asan`/`ci-linux-tsan` presets and the caveat that sanitizer/GCC/Clang CI jobs use manual flags, not presets.
  - Bloat-check `find` path generalized to `GameModules` (matches current `CLAUDE.md`; source pinned `GameModules/SparkGame/Source`).
  - Cross-references retargeted to the migrated wiki pages. Removed references to `clang-format.md`, `ai-bloat-pattern.md`, `codebase-observations.md` source files (not migrated to this folder).

## Related Pages

- [Git Rebase Conflicts](Git-Rebase-Conflicts.md)
- [Build Optimizations](Build-Optimizations.md)
- [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md)
- [CI Reproducible Builds](CI-Reproducible-Builds.md)
- [Project conventions (CLAUDE.md)](../../CLAUDE.md)
