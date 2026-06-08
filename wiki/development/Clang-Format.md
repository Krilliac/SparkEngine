# Clang-Format — CI-Matching Invocation

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (formatting is platform-independent; CI runs on Ubuntu)

## Overview

The CI `check-format` job runs clang-format over the full source tree on every push and pull request. Local pre-commit runs must scan the **same paths** as CI, or a local pass will be followed by a CI failure on files the local run never touched.

The single source of truth for formatting rules is `.clang-format` at the repo root. Never pass `--style=...` explicitly — clang-format discovers the root config automatically.

## What CI Actually Runs

The exact command lives in `.github/workflows/build.yml` under the `check-format` job. As of 2026-06-08 it is:

```bash
find SparkEngine/Source GameModules/SparkGame/Source GameModules/SparkGameMMO/Source \
     SparkEditor/Source SparkConsole/src SparkShaderCompiler/src SparkBuild/src SparkInstaller/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror > check-format-output.log 2>&1 || true
```

CI then **greps the log for the violation marker** rather than trusting the exit code:

```bash
if echo "$OUTPUT" | grep -q "\-Wclang-format-violations"; then
  exit 1
fi
```

This grep-based check exists because clang-format 18+ emits a spurious "missing Objective-C config" warning and returns exit code 1 even with zero violations. The `|| true` swallows that false failure; the grep finds real ones.

Note the path list has grown beyond the historical `SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules` set — it now scans specific GameModules subtrees (`SparkGame`, `SparkGameMMO`), plus `SparkBuild/src` and `SparkInstaller/src`, includes `*.hpp`, and excludes `*/Metal/*` (the Objective-C++ Metal sources are not C++-formatted).

## Wrong (commonly seen in old examples)

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
  -name '*.h' -o -name '*.cpp' \
  | head -50 \
  | xargs clang-format --dry-run --Werror
```

`head -50` caps the input to 50 files. Locally this passes while leaving thousands of files unchecked; CI runs without it and fails immediately. The path list is also stale and `*.hpp` is missing.

## Right — local check

For a quick local check, point clang-format at the directories you changed (or the full CI path list above):

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

- **Never use `head -N`** in a format check. If the argv is too long, batch with `xargs -n 500` instead.
- **`--Werror` is required** — without it, `--dry-run` only reports diffs and exits 0.
- **Exclude `*/Metal/*`** to match CI (Objective-C++ sources are not C++-formatted).
- **Include `*.hpp`** — CI does.
- **`.clang-format` at the repo root is the source of truth.** Don't pass `--style=...`.
- When you change formatting rules, match the `check-format` job in `.github/workflows/build.yml`.

## Source & Freshness

- **Original entry date:** 2026-04-17 (`.claude/knowledge/clang-format.md`, type: Pattern)
- **Verified against codebase 2026-06-08.**
- **UPDATED:** The CI `check-format` path list has expanded — it now scans `SparkGame`/`SparkGameMMO` GameModules subtrees, `SparkBuild/src`, and `SparkInstaller/src`, includes `*.hpp`, and excludes `*/Metal/*`. The old entry's path list was stale.
- **UPDATED:** CI no longer relies on the clang-format exit code — it captures output to a log, runs with `|| true`, and greps for `-Wclang-format-violations` (works around the clang-format 18+ Objective-C-config false failure). The old entry implied a raw exit-code check.
- **VERIFIED:** `--Werror` requirement, no `head -N`, no `--style=` override, `.clang-format` at repo root all still hold. CI command still lives in `.github/workflows/build.yml`.

## Related Pages

- [AI-Bloat-Pattern.md](AI-Bloat-Pattern.md) — anti-bloat philosophy, the other half of pre-commit hygiene
- [Code-Quality-Violations.md](Code-Quality-Violations.md) — function/method size limits enforced beyond formatting
- [MinGW-Wine-Cross-Compilation.md](MinGW-Wine-Cross-Compilation.md) — building Windows code on the same Linux CI runners
