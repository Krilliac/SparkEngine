# CI Reproducible Builds — Local Reproduction Commands

**Last updated:** 2026-03-30
**Type:** Pattern
**Status:** Active

## Description

Exact build commands to reproduce each CI job locally. The GitHub Actions workflow (`.github/workflows/build.yml`) runs these jobs. Use these when a CI check fails and you need to reproduce locally.

## Context

CI runs on every PR. When a job fails, use `gh run view <ID> --log-failed` to get logs, then reproduce locally using the matching command below.

## Approach

### clang-format check (runs on every PR)

```bash
find SparkEngine/Source GameModules SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1
# Fix: pipe the same file list to clang-format -i
```

### Linux GCC build (Debug + Release)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

### Linux Clang build (Debug + Release)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure && ./bin/SparkTests && cd ..
```

### Linux GCC AddressSanitizer + UBSan + LSan (Debug)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 LSAN_OPTIONS=suppressions=Tests/lsan_suppressions.txt \
  cd build && ./bin/SparkTests --output-file asan-results.txt && cd ..
```

### Linux GCC ThreadSanitizer (Debug)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build --parallel $(nproc)
TSAN_OPTIONS=halt_on_error=0 cd build && ./bin/SparkTests --output-file tsan-results.txt && cd ..
```

### Linux Clang MemorySanitizer (Debug)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -stdlib=libc++ -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_C_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++ -lc++abi" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++"
cmake --build build --parallel $(nproc)
MSAN_OPTIONS=halt_on_error=0 cd build && ./bin/SparkTests --output-file msan-results.txt && cd ..
```

### Windows MSVC VS 2022 (v143) (Debug + Release)

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DSPARK_MSVC_TOOLSET=v143 -DBUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

### Prompt validation (runs on every PR)

```bash
./tools/validate-prompts.sh --ci
```

## Notes

- For Windows-only failures that cannot be reproduced on Linux, inspect CI logs carefully and fix based on MSVC-specific diagnostics (e.g., `/W4` warnings, MSVC type conversion rules, Windows SDK headers)
- **See also:** [build-optimizations.md](build-optimizations.md) for CI workflow speedups
