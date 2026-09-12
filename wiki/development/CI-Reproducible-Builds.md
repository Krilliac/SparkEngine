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

### Trusted build-matrix verification

The Windows Shipping producer and the Linux trusted verifier must compute the same parity report. Recorded Windows paths are data: use Windows lexical path rules for their basename, absolute-path, containment, and provenance comparisons, regardless of the verifier host. Native filesystem checks remain appropriate only when inspecting files on the current host. A mismatch such as `producer=3 trusted=19` can therefore indicate a checker portability defect even when the Windows build succeeded; do not waive report equality or promote the readiness gates to resolve it.

`Build Matrix Verifier` accepts only `push` and `workflow_dispatch` builds on `Working`. Its workflow trigger filters the branch and its job conditions filter those event types. Pull-request and scheduled builds remain ordinary CI evidence and must not invoke the publication verifier as if they were accepted release sources. The verifier still independently checks repository, workflow, commit, run attempt, artifact, and receipt identity before publishing a status.

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

CI also runs CTest on the Windows/macOS matrix jobs; for a fail-closed local equivalent, use `ctest --test-dir build --output-on-failure --parallel --no-tests=error`. On the Linux GCC/Clang jobs it invokes `./bin/SparkTests` directly. Either is fine locally — CTest gives per-test isolation, `./bin/SparkTests` gives a single combined run.

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

MSan requires every linked C++ object, including the runtime, to be instrumented. CI therefore first builds an MSan-instrumented libc++/libc++abi from the LLVM 18.1.3 release tarball (hash-pinned in `build.yml`) with the recipe `.github/msan/libcxx-runtime.cmake`, caches that prefix by recipe hash and runner clang version, and then builds only the `SparkTests` target against it (`cmake --build build --parallel $(nproc) --target SparkTests`).

```bash
# 1. Instrumented runtime (once; CI caches the prefix). llvm-project-18.1.3.src.tar.xz from the llvmorg-18.1.3 release.
PREFIX=$PWD/msan-libcxx
cmake -G Ninja -S llvm-project-18.1.3.src/runtimes -B build-msan \
  -C .github/msan/libcxx-runtime.cmake \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_INSTALL_PREFIX=$PREFIX
cmake --build build-msan --target install-cxx install-cxxabi

# 2. SparkTests against it (no distro libc++-dev installed on the runner)
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DENABLE_SDL2=OFF \
  -DENABLE_VULKAN=OFF \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-recover=memory -fno-omit-frame-pointer -stdlib=libc++ -nostdinc++ -isystem $PREFIX/include/c++/v1 -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_C_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fsanitize-recover=memory -fno-omit-frame-pointer -fsanitize-ignorelist=$(pwd)/Tests/msan_ignorelist.txt" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++ -L$PREFIX/lib -Wl,-rpath,$PREFIX/lib -lc++abi" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=memory -stdlib=libc++ -L$PREFIX/lib -Wl,-rpath,$PREFIX/lib -lc++abi"
cmake --build build --parallel $(nproc) --target SparkTests
cd build
MSAN_OPTIONS=halt_on_error=1 ./bin/SparkTests --warn-is-error --shuffle 123
cd ..
```

The job stays `continue-on-error` (advisory) until a run classifies clean with the instrumented runtime; `Tests/TestMSanCanary.cpp` is designed to fail RED if the linked libc++ is not instrumented (RED proof pending the first run), and a failure before the tests run is labelled `infrastructure stage '<stage>' failed` in `ci-errors-linux-msan`. Practically, reproduce it only when CI flags a real MSan finding.

## Windows MSVC VS 2022 (v143) — Debug + Release (job `build-windows-vs2022`)

Run from an "x64 Native Tools Command Prompt for VS 2022" (or after `Enter-VsDevShell -VsInstallPath <vs> -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"`): Ninja needs `cl`/`rc` on PATH and `INCLUDE`/`LIB` set, and the SDK `fxc` on PATH is what lets the foliage shader validations run.

```bash
cmake --fresh -B build -G "Ninja Multi-Config" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl \
  -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
  -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DGENERATE_DEBUG_SYMBOLS=OFF \
  -DBUILD_TESTS=ON -DBUILD_GAME_MODULES=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure --parallel --no-tests=error
```

Without sccache installed, omit the two `_LAUNCHER` flags. With sccache, keep `-DGENERATE_DEBUG_SYMBOLS=OFF`: Jolt's own `/Zi` would otherwise make sccache expect a PDB that `cl` (honouring the engine's later `/Z7`) never writes, and every Jolt translation unit fails with `sccache: encountered fatal error`. PCH is off because sccache cannot cache `/Yc` / `/Fp`; module scanning is off because no module units exist.

CI installs sccache v0.17.0 from the GitHub release verified against a SHA-256 literal, keeps the cache in `SCCACHE_DIR` under `runner.temp` (split `actions/cache/restore` / `actions/cache/save` steps keyed like the Linux ccache steps; the save runs even when the job went red; `build/` is not restored), starts the server explicitly before configure, and reports the numbers in a `Print sccache stats` step. The engine selects embedded MSVC debug information (`/Z7`) through CMake policy CMP0141 before `project()`, which is what lets the configure-time try-compiles pass under the launcher.

The advisory `build-windows-vs2026` job follows the same recipe inside the VS 2026 developer environment (vswhere `[18.0,19.0)`, default v145 toolset) without `-DBUILD_GAME_MODULES=ON`; it fails visibly when that toolchain is absent instead of reporting a green no-op. `build-windows-shipping` still configures through the `windows-shipping` preset (Visual Studio 17 2022 generator, v143) and uses no compiler cache.

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
- `report-ci-errors` — aggregates `ci-errors-*` artifacts from failed jobs; findings from advisory lanes (job-level `continue-on-error`, e.g. `build-linux-msan`) are listed but do not fail the report. The reporter is loaded from the trusted `Working` checkout, so a change to it takes effect only after it lands there

## Notes

- For Windows-only failures that cannot be reproduced on Linux, inspect CI logs carefully and fix based on MSVC-specific diagnostics (e.g., `/W4` warnings, MSVC type-conversion rules, Windows SDK headers).
- `$(nproc)` is Linux/bash-specific; on macOS use `$(sysctl -n hw.logicalcpu)`.

## Source & Freshness

- Original entry: `CI Reproducible Builds — Local Reproduction Commands`, last updated 2026-03-30.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - **Fixed a broken shell construct** in the ASan/TSan/MSan run lines: the source wrote `ENV=... cd build && ./bin/SparkTests` which applies the env var to `cd`, not to the test binary. Rewritten as `cd build` then the env-prefixed `./bin/SparkTests` run, matching how CI actually invokes it.
  - Aligned the LSan suppressions path to `../Tests/lsan_suppressions.txt` (relative to `build/`); verified `Tests/lsan_suppressions.txt` and `Tests/msan_ignorelist.txt` exist.
  - Removed the source's prior CTest-plus-`SparkTests` combo from the GCC/Clang jobs — CI runs `./bin/SparkTests` directly there; clarified where CTest actually runs (Windows/macOS matrix).
  - Output filenames updated to match current CI (`asan-ubsan-lsan-results.txt`, etc.).
  - Added the new `ci-linux-asan` / `ci-linux-tsan` presets as alternatives.
  - Noted MSan builds only the `SparkTests` target in CI; added `|| true` to match CI (2026-09-06: the recipe now builds the MSan-instrumented libc++ first and runs with `halt_on_error=1`, so the `|| true` was dropped again).
  - Added sccache/`continue-on-error` notes for the Windows jobs and the v145 VS 2026 variant.
  - Windows VS 2022 / VS 2026 recipes switched to Ninja Multi-Config + sccache (2026-09-06); the Visual Studio-generator configure now applies only to `build-windows-shipping`'s preset.
  - Added the jobs that did not exist in the source: `check-thirdparty-manifest`, `coverage`, `clang-tidy`, `todo-count`, `build-installer`, `report-ci-errors`, plus the macOS and MinGW-Wine reproduction recipes.
  - Noted the Linux GCC job uses gcc-14/g++-14.

## Related Pages

- [GitHub API and PR Checks](GitHub-API-and-PR-Checks.md) — fetching the failing logs
- [Build Optimizations](Build-Optimizations.md) — CI workflow speedups
- [Workflow Patterns](Workflow-Patterns.md) — pre-push checklist
- [Project conventions (CLAUDE.md)](../../CLAUDE.md) — "CI jobs summary" table

## Stable release version contract

Versioned publication requires its `vMAJOR.MINOR.PATCH` tag to equal the single `SPARK_ENGINE_VERSION` default in `CMakeLists.txt`. `CHANGELOG.md` must contain exactly one matching `## [MAJOR.MINOR.PATCH]` heading, optionally followed by ` - YYYY-MM-DD`. Missing, duplicate, or mismatched metadata fails preparation before release outputs are emitted. Nightly publication continues to use the source default without requiring a versioned changelog section. This contract does not certify release notes, signing, or Windows qualification; the stable readiness gate remains mandatory.

Portable POSIX CPack archives keep SDL2's ABI-visible library names but
dereference its SONAME/development symlinks during install staging. Installed
executables search `lib/` through `$ORIGIN/../lib` on Linux and
`@executable_path/../lib` on macOS. The archive preflight therefore sees regular
files while the runtime loader and exported CMake targets retain their expected
names.

Installed-template live smokes also set `SPARK_TEMPLATE_LIVE_SMOKE_EVIDENCE=1`
inside a disposable project copy. The shared template bridge and FPSStarter's
compatible bridge write `.spark-template-live-smoke.log`; the verifier reads it
alongside process and FileSink output before removing the copy. This keeps the
scene-ownership gate reliable for GUI-subsystem Windows hosts whose module DLLs
have DLL-local logger state.

## Windows native-package component preflight

Versioned Windows publication stages only `runtime`, `tools`, and `samples` into a fresh root before CPack. The package validator's explicit `runtime` layout checks that SDK-free tree against the trusted build's generated game-module inventory and the validator checkout's canonical SDK ABI header. It retains the complete required executable and runtime-content lists, every configured module's sidecar/schema/ABI/SHA-256 checks, and executable help/version smokes. The default `sdk` layout retains its installed inventory/header checks. Unknown layouts, runtime use outside `stable-v1`, or reference files inside the installed package are rejected.

The workflow retains `runtime-layout.log` on success and failure. Local fixtures test orchestration and rejection behavior; POSIX script stand-ins are not engine binaries. This preflight does not execute an MSI or NSIS installer and does not certify Windows 11, code signing, upgrades, rollback, or uninstall. Native installation qualification remains required.

## Native MSI install/uninstall qualification

The existing Windows Shipping CI job packages its already-built MinSizeRel tree with WiX and runs `.github/scripts/qualify-windows-msi.py`; versioned publication invokes the same gate after CPack and before artifact upload. The gate requires exactly one expected MSI filename and matching read-only database identity, refuses an already registered product or any registered product sharing its UpgradeCode, hashes the selected artifact, and installs into a unique directory under the runner temporary directory. After installation, it requires Windows Installer to report `INSTALLSTATE_DEFAULT` (5) for the selected product; absent, advertised, or broken registration fails before runtime execution, with uninstall still attempted. It validates the actual installed runtime layout and runs a five-frame installed SparkGameFPS headless smoke with module initialization required and a positive `SPARK_MODULE_READY` marker. This verifies startup/lifecycle, not interactive gameplay or an authored-scene acceptance test.

Uninstall is attempted in `finally` with the same MSI unless its bytes changed, preserving the original error if cleanup also fails. Reboot-required and other nonzero installer exits fail. Remaining product registration or install-root residue also fail; the script never conceals failure by recursively deleting installed files. Logs and a commit/artifact-bound result are retained separately from build-matrix evidence. Local injected-process fixtures establish orchestration behavior only. Actual hosted native results remain pending until CI runs; Windows Server evidence does not certify Windows 11, D3D11, signing, NSIS, upgrade, rollback, or user-data retention.

The install-directory override is `INSTALL_ROOT`, matching [CMake's WiX template](https://gitlab.kitware.com/cmake/cmake/-/blob/release/Modules/Internal/CPack/WIX-v3/WIX.template.in). The package's Directory table is checked before use. Microsoft documents [quiet install/uninstall, no-restart, logging, and public properties](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/msiexec) and [read-only MSI database access](https://learn.microsoft.com/en-us/windows/win32/msi/installer-opendatabase).

## Stable Windows outer installer signature gate

Versioned Windows publication runs `.github/scripts/verify-windows-package-signatures.py` after CPack and before native MSI installation or package upload. It requires exactly the expected versioned MinSizeRel Runtime EXE and MSI at the package directory's top level, rejects links/nonregular files and extra EXE/MSI entries, and uses the system Windows PowerShell [Get-AuthenticodeSignature](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.security/get-authenticodesignature?view=powershell-5.1) command. Microsoft documents that catalog signatures take precedence when present; this gate rejects that result. Both installers must report `Valid`, an embedded `Authenticode` signature (catalog signatures are insufficient), a timestamp certificate, and the explicit publisher certificate thumbprint configured in the repository variable `SPARK_RELEASE_SIGNER_THUMBPRINT` (40 hexadecimal characters). Certificate thumbprints identify the approved certificate; artifact content is bound separately with SHA-256.

Owners must provision the trusted publisher certificate identity and a secure signing pipeline for the final CPack outputs before this gate can pass. The workflow does not sign artifacts, invent a publisher identity, or accept an empty configuration. Current unsigned CPack outputs therefore block stable publication. Nightly packaging and ordinary Shipping CI installer qualification are unchanged.

The diagnostic JSON records the exact source commit, artifact SHA-256 values, publisher and timestamp certificate subjects/thumbprints, native signature status, and pass/failure. Each file is hashed before and after the native signature query; a second workflow check requires the same bytes after MSI qualification and immediately before package upload. Diagnostics are retained on failure as well as success. Injected-process tests cover rejected status, catalog signatures, mismatched signer, absent timestamp, malformed responses, process failures, file substitution, and selection defects. Windows-only unsigned-fixture subtests exercise the real PowerShell command independently for EXE and MSI in ordinary VS 2022 Release PR CI before compilation and early in the stable release job; local Linux runs skip them. A second-artifact-only rejection fixture also proves that a valid EXE result cannot hide an unsigned MSI.

This is a bounded first slice of REL-110, which remains open. It verifies only the outer NSIS EXE and MSI; it does not verify ZIP contents or internal PE payload signatures, prove NSIS installation behavior, provide independent consumer verification, or complete signing, scanning, protected approval, or Windows 11 qualification. Native signed-artifact success evidence remains pending the owner's signing setup and hosted run.
