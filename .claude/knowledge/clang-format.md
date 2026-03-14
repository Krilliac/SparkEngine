# clang-format Issues and Fixes

**Last updated:** 2026-03-14
**Type:** Issue
**Status:** Resolved

## Issue

The `check-format` CI job fails even after running the clang-format command from CLAUDE.md's pre-commit checklist. The pre-commit shortcut only checks 50 files, while CI checks all files — so format errors in file 51+ pass the local check but fail CI. Additionally, CI excludes Metal backend files while some local commands do not.

## Context

Occurs when:
1. Running the pre-commit check from CLAUDE.md (step 1 uses `head -50`)
2. Editing files in the Metal backend path (CI excludes these, local commands may not)
3. Using an older clang-format version than CI uses (ubuntu-24.04 ships a specific version)

The CI job `check-format` runs on `ubuntu-24.04` and scans all source files.

## Methods Tried

1. **`find ... | head -50 | xargs clang-format --dry-run --Werror`** (from CLAUDE.md pre-commit step 1) → INCOMPLETE
   Reason: Only checks the first 50 files by modification time. Files 51+ are never checked. This is noted in CLAUDE.md as a "format check" but it's actually a partial check. Passes locally, fails CI.

2. **Running clang-format without Metal exclusion** → PRODUCES FALSE POSITIVES
   Reason: Metal backend files (in `*/Metal/*`) use Objective-C++ conventions that don't match the project's `.clang-format` rules. CI excludes them with `-not -path '*/Metal/*'`; local runs without this exclusion will report false format errors in Metal files.

3. **Full CI-matching command** (below) → CORRECT

## Solution (Reliable Method)

Always use the **exact CI command** for both checking and fixing:

```bash
# CHECK (dry-run, matches CI exactly):
find SparkEngine/Source SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1

# FIX (auto-format, same file set):
find SparkEngine/Source SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format -i
```

Key differences from the CLAUDE.md pre-commit shortcut:
- **No `head -50`** — scans all files
- **Includes `-not -path '*/Metal/*'`** — excludes Metal backend, matching CI
- **Includes `.hpp` files** — CI checks `.hpp` too, the shortcut only does `.h` and `.cpp`

## Notes

- The `.clang-format` config uses `BasedOnStyle: Microsoft` with Allman braces, 4-space indent, 120-column limit.
- If clang-format on your machine produces different results than CI, check your clang-format version: `clang-format --version`. CI uses the version shipped with ubuntu-24.04 (typically clang-format 18).
- Metal backend files (`SparkEngine/Source/Graphics/RHI/Metal/`) are intentionally excluded from format enforcement — do not try to format them.
- The `find` command pipes to `xargs` which may fail silently if no files are found. If the command produces no output, verify the source paths are correct from the repo root.
- After running `clang-format -i`, always re-run the dry-run check to confirm no files were missed.
