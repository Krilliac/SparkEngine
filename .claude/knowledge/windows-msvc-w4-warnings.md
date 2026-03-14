# Windows MSVC /W4 Warnings-as-Errors

**Last updated:** 2026-03-14
**Type:** Issue
**Status:** Resolved

## Issue

The `build-windows-vs2022` CI job compiles with `/W4` and treats warnings as errors. Code that compiles cleanly on Linux under GCC/Clang may fail on Windows due to MSVC-specific diagnostics: narrowing conversions, unreferenced parameters, and member-hides-member naming conflicts. These failures are invisible until the Windows CI job runs.

## Context

Occurs in the `build-windows-vs2022` CI job (MSVC v143, blocking). The `build-windows-vs2026` job (MSVC v144) uses `continue-on-error: true` — failures there are informational only.

## Methods Tried

1. **`#pragma warning(disable: CXXX)`** → NOT ACCEPTABLE
   Violates the project's "zero warnings" standard. Do not use.

2. **`(void)param;` to suppress C4100 (unreferenced parameter)** → WORKS but suboptimal
   Better: use `[[maybe_unused]]` — more expressive, C++20 style.

3. **Using `int` where MSVC expects `size_t`** → CAUSES C4267
   Fix: `static_cast<int>(container.size())`.

4. **Per-warning fix table** (below) → CORRECT

## Solution (Reliable Method)

When Windows CI fails but Linux passes, retrieve the failure log and apply the fix table:

```bash
# 1. Get the run ID
gh run list --branch "$(git branch --show-current)" --limit 5

# 2. Download failure log
gh run view <RUN_ID> --log-failed
# Look for lines like: error C4267: 'argument': conversion from 'size_t' to 'int'

# 3. Apply fix from table, commit, push, re-poll
gh pr checks <PR_NUMBER>
```

**Fix table for recurring MSVC /W4 warnings:**

| Warning | Meaning | Correct Fix |
|---------|---------|-------------|
| `C4244` | Narrowing: `double` → `float` | `static_cast<float>(value)` |
| `C4267` | Narrowing: `size_t` → `int` | `static_cast<int>(container.size())` |
| `C4100` | Unreferenced formal parameter | `[[maybe_unused]] ParamType paramName` |
| `C4189` | Local variable initialized but unused | Remove the variable or use it |
| `C4458` | Local declaration hides class member | Rename the local (e.g., add `local` prefix) |
| `C4701` | Potentially uninitialized local variable | Initialize to a default value at declaration |
| `C4702` | Unreachable code | Remove dead code |

## Notes

- Windows-only code (DirectX, XAudio2) must be inside `#ifdef SPARK_PLATFORM_WINDOWS` or `#ifdef _WIN32` guards. Warnings inside these blocks cannot be reproduced on Linux — diagnose from CI logs only.
- `/W4` is set in `CMakeLists.txt` for MSVC. Do not add `/WX` manually — it is already implied by the CI job configuration.
- Prefer `static_cast<>` over C-style casts throughout — project standard is C++20 style.
- `[[maybe_unused]]` is preferred over `(void)param;` for C4100.
- `build-windows-vs2026` (MSVC v144) uses `continue-on-error: true` — do not block a PR on its failures unless explicitly asked.
