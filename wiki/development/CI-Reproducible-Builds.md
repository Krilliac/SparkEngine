# CI Reproducible Builds — Local Reproduction Commands

> **Audience:** Programmers
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** Linux (GCC/Clang, sanitizers, MinGW-Wine), Windows (MSVC), macOS

## Overview

Exact build commands to reproduce each CI job locally. The GitHub Actions workflow (`.github/workflows/build.yml`) runs these jobs on every PR. Use these when a CI check fails and you need to reproduce locally.

When a job fails, use `gh run view <RUN_ID> --log-failed` to get logs (see [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md)), then reproduce locally using the matching command below.

The CI test binary is `SparkTests` (built into `build/bin/`). CI runs it directly (`./bin/SparkTests`) and, for the standard GCC/Clang/Windows/macOS matrix jobs, via `ctest`. The Linux GCC job uses GCC 14 (`gcc-14`/`g++-14`).

## clang-format check (runs on every PR — job `check-format`)

```bash
find SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1
# Fix: pipe the same file list to clang-format -i
```

## Linux GCC build — Debug + Release (job `build-linux-gcc`)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ./bin/SparkTests && cd ..
```

CI also runs `ctest --test-dir build --output-on-failure --parallel` on the Windows/macOS matrix jobs; on the Linux GCC/Clang jobs it invokes `./bin/SparkTests` directly. Either is fine locally — `ctest` gives per-test isolation, `./bin/SparkTests` gives a single combined run.

## Linux Clang build — Debug + Release (job `build-linux-clang`)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel $(nproc)
cd build && ./bin/SparkTests && cd ..
```

## Linux GCC AddressSanitizer + UBSan + LSan — Debug (job `build-linux-asan`)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 \
  LSAN_OPTIONS=suppressions=../Tests/lsan_suppressions.txt \
  ./bin/SparkTests --output-file asan-ubsan-lsan-results.txt
cd ..
```

There is also a `ci-linux-asan` CMake preset that bundles these flags if you prefer `cmake --preset ci-linux-asan`.

## Linux GCC ThreadSanitizer — Debug (job `build-linux-tsan`)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build --parallel $(nproc)
cd build
TSAN_OPTIONS=halt_on_error=0 ./bin/SparkTests --output-file tsan-results.txt
cd ..
```

A `ci-linux-tsan` CMake preset bundles these flags as well.

## Linux Clang MemorySanitizer — Debug (job `build-linux-msan`, `continue-on-error`)

CI builds only the `SparkTests` target for this job (`cmake --build build --parallel $(nproc) --target SparkTests`).

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -stdlib=libc++ -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_C_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++ -lc++abi" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++"
cmake --build build --parallel $(nproc) --target SparkTests
cd build
MSAN_OPTIONS=halt_on_error=0 ./bin/SparkTests --output-file msan-results.txt || true
cd ..
```

MSan requires libc++ built with `-fsanitize=memory`; this is why the job is `continue-on-error`. Practically, reproduce it only when CI flags a real MSan finding.

## Windows MSVC VS 2022 (v143) — Debug + Release (job `build-windows-vs2022`)

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSPARK_MSVC_TOOLSET=v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --parallel
```

CI additionally passes `-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache` for caching; omit those locally unless you have sccache installed. The VS 2026 job (`build-windows-vs2026`, `continue-on-error`) is identical but uses `-T v144 -DSPARK_MSVC_TOOLSET=v144`.

## macOS (job `build-macos`, `continue-on-error`)

```bash
cmake --preset macos-release
cmake --build build --parallel $(sysctl -n hw.logicalcpu)
cd build && ./bin/SparkTests && cd ..
```

## MinGW + Wine (job `build-linux-mingw-wine`, `continue-on-error`)

Cross-compiles the Windows D3D11 code on Linux and runs it under Wine:

```bash
cmake --preset linux-mingw-release
cmake --build build --parallel $(nproc)
tools/wine-run.sh build/bin/SparkTests.exe
```

See the project's MinGW/Wine setup notes for the full toolchain install (`tools/setup-mingw-wine.sh`).

## Prompt validation (runs on every PR — job `validate-prompts`)

```bash
./tools/validate-prompts.sh --ci
```

## Other CI jobs

- `check-thirdparty-manifest` — `./tools/check-thirdparty-manifest-sync.sh`
- `coverage` — GCC Debug with `--coverage` + lcov, per-subsystem thresholds
- `clang-tidy` (`continue-on-error`) — Clang Debug static analysis
- `todo-count` — fails if TODO count exceeds threshold (20)
- `build-installer` — builds the `SparkInstaller` target
- `report-ci-errors` — aggregates `ci-errors-*` artifacts from failed jobs

## Notes

- For Windows-only failures that cannot be reproduced on Linux, inspect CI logs carefully and fix based on MSVC-specific diagnostics (e.g., `/W4` warnings, MSVC type-conversion rules, Windows SDK headers).
- `$(nproc)` is Linux/bash-specific; on macOS use `$(sysctl -n hw.logicalcpu)`.

## Source & Freshness

- Original entry: `CI Reproducible Builds — Local Reproduction Commands`, last updated 2026-03-30.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - **Fixed a broken shell construct** in the ASan/TSan/MSan run lines: the source wrote `ENV=... cd build && ./bin/SparkTests` which applies the env var to `cd`, not to the test binary. Rewritten as `cd build` then the env-prefixed `./bin/SparkTests` run, matching how CI actually invokes it.
  - Aligned the LSan suppressions path to `../Tests/lsan_suppressions.txt` (relative to `build/`); verified `Tests/lsan_suppressions.txt` and `Tests/msan_ignorelist.txt` exist.
  - Removed the source's `ctest --output-on-failure && ./bin/SparkTests` combo from the GCC/Clang jobs — CI runs `./bin/SparkTests` directly there; clarified where `ctest` actually runs (Windows/macOS matrix).
  - Output filenames updated to match current CI (`asan-ubsan-lsan-results.txt`, etc.).
  - Added the new `ci-linux-asan` / `ci-linux-tsan` presets as alternatives.
  - Noted MSan builds only the `SparkTests` target in CI; added `|| true` to match CI.
  - Added sccache/`continue-on-error` notes for the Windows jobs and the v144 VS 2026 variant.
  - Added the jobs that did not exist in the source: `check-thirdparty-manifest`, `coverage`, `clang-tidy`, `todo-count`, `build-installer`, `report-ci-errors`, plus the macOS and MinGW-Wine reproduction recipes.
  - Noted the Linux GCC job uses gcc-14/g++-14.

## Related Pages

- [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md) — fetching the failing logs
- [Build Optimizations](Build-Optimizations.md) — CI workflow speedups
- [Workflow Patterns](Workflow-Patterns.md) — pre-push checklist
- [Project conventions (CLAUDE.md)](../../CLAUDE.md) — "CI jobs summary" table
