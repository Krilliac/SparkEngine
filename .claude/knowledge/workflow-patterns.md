# Effective SparkEngine Development Workflows

**Last updated:** 2026-03-14
**Type:** Pattern
**Status:** Active

## Description

Recurring workflows that have proven consistently effective for SparkEngine development tasks. These patterns reduce time spent on overhead and increase the reliability of outputs. Apply them proactively rather than waiting until something goes wrong.

## Context

Applies to all SparkEngine development sessions. Most patterns correspond to gaps where the "obvious" approach is slower or less reliable than the pattern described here.

---

## Pattern: Codebase Exploration — Parallel Explore Agents

### Approach

When a task touches multiple areas of the codebase (e.g., adding a feature that spans ECS + Graphics + Editor), launch 2–3 Explore agents **in parallel** rather than sequentially. Each agent gets a specific search focus.

```
Agent 1: "Search for existing implementations of X in SparkEngine/Source/Engine/ECS/"
Agent 2: "Find all usages of Y in SparkEditor/Source/"
Agent 3: "Identify patterns for Z in Tests/"
```

Do not use a single Explore agent for a broad multi-area search — it will either miss areas or spend too long on one area.

### When to apply

- Task involves code in 2+ directories
- Scope is uncertain and you need to map the codebase before planning
- Looking for an existing implementation before writing new code (always check first)

### Notes

- Use `subagent_type: Explore` for read-only codebase research
- Use `subagent_type: Plan` when you need architectural design work after exploration
- 3 agents maximum per parallel batch; quality over quantity

---

## Pattern: Documentation Sync After Structural Changes

### Approach

After any change that adds, renames, or removes public headers, ECS components, systems, editor panels, or tests — run both doc scripts before committing:

```bash
docs/generate-api-docs.sh check    # Regenerates API pages if headers changed
docs/sync-wiki.sh sync             # Updates wiki AUTO: sections (component/system/test counts)
```

Then `git add` any changed files under `docs/api/` or `wiki/` alongside the code change.

### When to apply

- Added or removed a `.h` file
- Added or removed an ECS component or system
- Added or removed an editor panel
- Added or removed a test

### Notes

- If you forget and CI catches it, the `check-format` job will not catch this — it only checks C++ formatting. The stale docs will appear as unexpected diffs in subsequent sessions.
- `docs/generate-api-docs.sh check` uses checksums — it's fast when nothing changed.
- `docs/sync-wiki.sh sync` updates `<!-- AUTO:* -->` markers in wiki files. Never edit those markers manually.

---

## Pattern: Pre-Push Checklist Order

### Approach

Run pre-push checks in this specific order — each step catches different error classes and the order minimizes wasted time:

```bash
# 1. Format first (fastest, most common failure)
find SparkEngine/Source SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1

# 2. CMake configure (catches missing headers, broken includes)
cmake --preset linux-gcc-release 2>&1 | tail -20

# 3. Build (catches compile errors)
cmake --build build --config Release --parallel $(nproc) 2>&1 | tail -30

# 4. Tests (catches regressions)
cd build && ctest --output-on-failure && cd ..

# 5. Docs (catches stale auto-generated content)
docs/generate-api-docs.sh check
docs/sync-wiki.sh sync
```

Stop at the first failure; fix it before proceeding to the next step.

### Notes

- Step 1 (format) is cheap (~5 seconds). Do it even for tiny changes — MSVC code often reformats differently.
- Step 2 (configure) detects include path issues early before a full compile.
- Step 5 (docs) is only needed when step 1 touches headers or structural files.
- **See also:** [clang-format.md](clang-format.md) for why `head -50` must not be used in step 1; [build-optimizations.md](build-optimizations.md) for the `--parallel $(nproc)` flag in step 3.

---

## Pattern: CMake Preset Over Manual Flags

### Approach

Always prefer `cmake --preset linux-gcc-release` over manually constructing `-B build -DCMAKE_BUILD_TYPE=...` flags.

```bash
# Preferred
cmake --preset linux-gcc-release

# Only use manual flags when a custom toggle is needed
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DENABLE_NETWORKING=ON \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
```

### Notes

- Presets are defined in `CMakePresets.json` and are authoritative — they match CI's exact configuration.
- If a preset produces unexpected behavior, check `CMakePresets.json` for the exact flags it sets rather than guessing.
- Delete `build/` if switching between preset and manual configure — the cache will conflict.

---

## Pattern: Session Start Checklist Order

### Approach

Every session should start in this exact order before doing anything else:

1. `git fetch origin Working && git log --oneline HEAD..origin/Working | wc -l` — assess how far behind
2. `git rebase origin/Working` — sync with upstream
3. Resolve any rebase conflicts (see [git-rebase-conflicts.md](git-rebase-conflicts.md))
4. `cat .claude/index.md` — load persistent context
5. Read any knowledge files relevant to the current task
6. **Then** start reading code or planning

Skipping step 4–5 means starting each session without the accumulated knowledge from prior sessions.

### Notes

- If `wc -l` output is 0 in step 1, the branch is up to date — skip the rebase.
- Never start reading or editing code before completing the git sync. Stale code leads to conflicts on push.
