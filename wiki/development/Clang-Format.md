# Clang-Format — CI-Matching Invocation

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (formatting is platform-independent; CI runs on Ubuntu)

## Overview

The CI `check-format` job enforces clang-format on changed C++ files across the full engine surface on every push and pull request. It compares the pushed range or pull-request base to `HEAD`, so legacy formatting debt does not conceal new debt and does not require unrelated mass rewrites.

The single source of truth for formatting rules is `.clang-format` at the repo root. Never pass `--style=...` explicitly — clang-format discovers the root config automatically.

## What CI Actually Runs

The exact command lives in `.github/workflows/build.yml` under the `check-format` job. Its source roots are:

```bash
SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
SparkBuild/src SparkInstaller/src SparkDaemon/src SparkServer/src SparkGateway/src \
SparkCooker/src SparkWorker/src SparkAutomation/src SparkLauncher/src Tests
```

CI obtains changed paths with `git diff --name-only --diff-filter=ACMR -z <base> HEAD`, keeps `*.h`, `*.hpp`, and `*.cpp`, excludes `*/Metal/*`, and passes that bounded list directly to clang-format.

CI then **greps the log for the violation marker** rather than trusting the exit code:

```bash
if echo "$OUTPUT" | grep -q "\-Wclang-format-violations"; then
  exit 1
fi
```

The workflow records the formatter exit code, verifies every source root and the tool first, fails on real violation markers, and also rejects abnormal formatter exits. A normal exit 1 without a violation marker remains tolerated because clang-format 18+ can emit a spurious "missing Objective-C config" warning.

The path list covers the engine, every game module, editor, standalone processes and tools, launcher, and tests; it includes `*.hpp` and excludes `*/Metal/*` (the Objective-C++ Metal sources are not C++-formatted). A docs-only change therefore performs no formatter invocation and succeeds explicitly.

## Wrong (commonly seen in old examples)

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
  -name '*.h' -o -name '*.cpp' \
  | head -50 \
  | xargs clang-format --dry-run --Werror
```

`head -50` caps the input to 50 files. Locally this passes while leaving thousands of files unchecked; CI runs without it and fails immediately. The path list is also stale and `*.hpp` is missing.

## Right — local check

For a local check that mirrors CI, format the C++ files changed from your base branch. For a quick manual check, point clang-format at the directories you changed:

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
     SparkBuild/src SparkInstaller/src GameModules \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) \
  | xargs clang-format --dry-run --Werror
```

## Fix Formatting in Place

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
     SparkBuild/src SparkInstaller/src GameModules \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) \
  | xargs clang-format -i
```

## Rules

- **Never use `head -N`** in a format check. If the argv is too long locally, batch without truncating the inventory.
- **`--Werror` is required** — without it, `--dry-run` only reports diffs and exits 0.
- **Exclude `*/Metal/*`** to match CI (Objective-C++ sources are not C++-formatted).
- **Include `*.hpp`** — CI does.
- **`.clang-format` at the repo root is the source of truth.** Don't pass `--style=...`.
- When you change formatting rules, match the `check-format` job in `.github/workflows/build.yml`.

## Source & Freshness

- **Original entry date:** 2026-04-17 (`.claude/knowledge/clang-format.md`, type: Pattern)
- **Verified against codebase 2026-06-08.**
- **UPDATED:** The CI `check-format` path list has expanded — it now scans `SparkGame`/`SparkGameMMO` GameModules subtrees, `SparkBuild/src`, and `SparkInstaller/src`, includes `*.hpp`, and excludes `*/Metal/*`. The old entry's path list was stale.
- **UPDATED:** CI validates every configured source root and clang-format installation, preserves the Objective-C warning workaround, and incrementally enforces all engine/module/tool/test surfaces without failing on unrelated legacy formatting debt.
- **VERIFIED:** `--Werror` requirement, no `head -N`, no `--style=` override, `.clang-format` at repo root all still hold. CI command still lives in `.github/workflows/build.yml`.

## Related Pages

- [AI-Bloat-Pattern.md](AI-Bloat-Pattern.md) — anti-bloat philosophy, the other half of pre-commit hygiene
- [Code-Quality-Violations.md](Code-Quality-Violations.md) — function/method size limits enforced beyond formatting
- [MinGW-Wine-Cross-Compilation.md](MinGW-Wine-Cross-Compilation.md) — building Windows code on the same Linux CI runners
