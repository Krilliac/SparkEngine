# CI Build Failure Patterns

**Last updated:** 2026-03-14
**Status:** Resolved

## Issue

CI failures come from 9+ different jobs with different compilers, OSes, and flags. Without knowing which jobs are blocking vs. optional, and without knowing how to reproduce failures locally, diagnosing CI failures takes much longer than necessary.

## Context

After every `git push` to a PR, GitHub Actions runs all CI jobs. Some failures block the PR merge; others are informational only. The full CI config lives at `.github/workflows/build.yml`.

## Blocking vs. Non-Blocking Jobs

**Blocking (must fix before merging):**

| Job | What it checks |
|-----|----------------|
| `check-format` | clang-format compliance — ALL source files |
| `validate-prompts` | `.github/prompts/` files via `tools/validate-prompts.sh --ci` |
| `build-linux-gcc` | GCC Debug + Release, runs tests |
| `build-linux-clang` | Clang Debug + Release, runs tests |
| `build-linux-asan` | ASan + UBSan Debug build |
| `build-windows-vs2022` | MSVC v143 Debug + Release |
| `coverage` | Code coverage (lcov) |
| `todo-count` | Fails if TODOs in source exceed 20 |

**Non-blocking (`continue-on-error: true`):**

| Job | Notes |
|-----|-------|
| `build-windows-vs2026` | MSVC v144 — failures are warnings only |
| `clang-tidy` | Static analysis — failures are warnings only |

## Methods Tried

1. **Ignoring non-blocking failures** → CORRECT — Don't debug `build-windows-vs2026` or `clang-tidy` unless explicitly asked.

2. **Trying to reproduce Windows failures on Linux** → FAILS for MSVC-specific issues. Windows-only failures (type narrowing under `/W4`, MSVC-specific headers, `__declspec` usage) must be diagnosed from CI logs, not local Linux builds.

3. **`gh pr checks`** → PARTIALLY WORKS for seeing which jobs failed. Use `gh run list` + `gh run view <RUN_ID> --log-failed` for actual log content. See [github-api-pr-checks.md](github-api-pr-checks.md).

## Solution (Reliable Diagnosis Workflow)

```bash
# 1. Get the run ID for the latest push
gh run list --branch "$(git branch --show-current)" --limit 3

# 2. View which jobs failed
gh run view <RUN_ID>

# 3. Download logs for failed jobs
gh run view <RUN_ID> --log-failed

# 4. Reproduce locally using the EXACT CI flags for the failing job (see below)
```

### Local Reproduction Commands by Job

**`check-format`** (most common failure):
```bash
find SparkEngine/Source SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1
# Fix:
find SparkEngine/Source SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format -i
```

**`build-linux-gcc`** (Debug + Release):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**`build-linux-clang`** (Debug + Release):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**`build-linux-asan`** (Debug):
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

**`todo-count`** (threshold: 20 TODOs):
```bash
grep -r "TODO" SparkEngine/Source SparkEditor/Source SparkGame/Source \
  --include="*.h" --include="*.cpp" | wc -l
```

**`validate-prompts`**:
```bash
./tools/validate-prompts.sh --ci
```

## Common Root Causes

| Symptom | Likely Cause |
|---------|-------------|
| `check-format` fails | Forgot to run `clang-format -i` before committing, or used `head -50` shortcut that missed files |
| GCC/Clang build fails but MSVC (hypothetically) would pass | GCC `-Wall -Wextra` catches more: unused variables, missing `[[nodiscard]]`, implicit conversions |
| ASan fails | Use-after-free, heap buffer overflow, or undefined behavior in new code |
| `todo-count` fails | Accumulated too many `// TODO` comments — remove or resolve some |
| Windows MSVC build fails (in CI logs) | Type narrowing (`int` → `DWORD`), missing `#include <windows.h>` ordering, `__declspec` usage |

## Notes

- Always fix `check-format` first — it's the fastest to diagnose and fix, and it unblocks other diagnostic work.
- After pushing a fix, wait 15+ seconds before polling again — GitHub needs time to start the new run.
- If `build-linux-gcc` passes but `build-linux-clang` fails, look for Clang-specific warnings: `-Wshadow`, `-Wimplicit-fallthrough`, or stricter template errors.
