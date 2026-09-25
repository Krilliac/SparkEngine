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

The `--output-file` name above is only a local convenience. CI does not write `asan-ubsan-lsan-results.txt`. It runs the suite through `.github/scripts/run-sanitizer-tests.sh`, which writes `junit.xml`, `metadata.json`, `console.txt`, `report.txt` and `process-footer.txt` into `$SANITIZER_EVIDENCE_DIR`, and uploads that directory as `test-results-linux-asan`. The `module-evidence` job (RDY-010) downloads it to `build/module-evidence/sanitizer-asan/` and runs `verify-sanitizer-evidence.py verify-published` on it. `tools/module-evidence/validate_manifest.py` then consumes it as SparkGameFPS's required `sanitizer-report` evidence: the metadata must name the exact commit, carry an origin directory that matches that commit and its recorded run id/attempt, be a clean run with zero failures, and match the `junit.xml` digest; the `junit.xml` must record at least the 6900-testcase SparkTests floor (`SANITIZER_MIN_JUNIT_TESTCASES`, pinned by a test to the step's `--minimum-tests`); and at least one `FPSRespawn_*` production-source test must have executed and passed. Only the preceding `verify-published` step binds the directory to the current workflow run id and attempt, so keep it ahead of the consumer.

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

Use a fresh build directory for each CMake executable, generator, compiler, or
toolset. Do not run two configure processes against the same directory. A
partially written compiler-information file can retain the compiler identity
while losing the ABI and `CMAKE_CXX_COMPILE_FEATURES` results; the later
`target_compile_features` diagnostic is then a stale-state symptom, not proof
that MSVC lacks C++23 support. Re-run with `cmake --fresh` (or remove the
directory) after changing the CMake installation or recovering an interrupted
configure.

CI installs sccache v0.17.0 from the GitHub release verified against a SHA-256 literal, keeps the cache in `SCCACHE_DIR` under `runner.temp` (split `actions/cache/restore` / `actions/cache/save` steps keyed like the Linux ccache steps; the save runs even when the job went red; `build/` is not restored), starts the server explicitly before configure, and reports the numbers in a `Print sccache stats` step. The engine selects embedded MSVC debug information (`/Z7`) through CMake policy CMP0141 before `project()`, which is what lets the configure-time try-compiles pass under the launcher.

The advisory `build-windows-vs2026` job follows the same recipe inside the VS 2026 developer environment (vswhere `[18.0,19.0)`, default v145 toolset) without `-DBUILD_GAME_MODULES=ON`; it fails visibly when that toolchain is absent instead of reporting a green no-op. `build-windows-shipping` still configures through the `windows-shipping` preset (Visual Studio 17 2022 generator, v143) and uses no compiler cache.

The build-matrix provenance checker records the absolute C++ compiler path in
addition to the Visual Studio generator instance, archiver, and linker. It
accepts only an absolute Windows x64 MSVC `cl.exe` path whose concrete toolset
version agrees with the other recorded tools. This is provenance validation,
not hosted reproducibility evidence: BLD-100 still requires two clean Windows
Shipping builds and an externally verified comparison.

### Shipping private symbols (BLD-100)

`STRIP_DEBUG_SYMBOLS=ON` (both Shipping presets) keeps private symbols out of
the runtime package. It no longer stops them from being produced:

- **MSVC**: every image links with `/DEBUG` (objects already compile with
  `/Z7`), so it carries a CodeView RSDS record, the PDB GUID and age that
  identify its PDB. Outside Debug, `/PDBALTPATH:%_PDB%` records only the PDB
  file name. `/OPT:REF` and `/OPT:ICF` are restated for every non-Debug
  configuration, because `/DEBUG` alone would switch them off.
- **ELF (GCC/Clang)**: every image links with `-Wl,--build-id=sha1`. With
  `STRIP_DEBUG_SYMBOLS=ON`, every target compiles with `-g` (the static
  libraries hold most shipped code), and each shipped image target gets
  `cmake/SparkSplitDebugLink.cmake` as its C/C++ `LINKER_LAUNCHER`. It runs the
  link, then writes `<image>.debug` (`objcopy --only-keep-debug`) and strips the
  image with a `.gnu_debuglink`. It runs inside the link step, so the module
  `.sparkabi` hash and every `POST_BUILD` copy see the stripped image. Every
  other image (SparkTests, test probes) links outside Debug with `-g0` and
  `--strip-all` and gets no `.debug`; under LTO, link-time `-g0` keeps the
  LTRANS stage from generating debug info (checked with GCC 13). MinGW keeps
  `-s`, and Apple has no strip step.

The PDBs and `.debug` files of the shipped image targets
(`SPARK_SHIPPED_IMAGE_TARGETS` in the root `CMakeLists.txt`) install only into
the `symbols` component. `CPACK_COMPONENTS_ALL` leaves that component out, so no
package carries symbols:

```bash
cmake --install <build> --component runtime --prefix stage     # also tools, samples
cmake --install <build> --component symbols --prefix symbols-stage
python3 tools/shipping_symbol_manifest.py --images stage \
    --symbols symbols-stage/symbols --output shipping-symbol-manifest.json
```

The manifest tool uses only the standard library. It maps each ELF image by
build-id, `.gnu_debuglink` name and CRC-32 to one DWARF `.debug` file, and each
PE image by RSDS GUID and age to one PDB: the GUID comes from the PDB info
stream and the age from the DBI stream. It writes a closed
`spark.shipping-symbol-manifest/1` JSON and writes nothing (exit 1) when any of
these hold:

- an image has no build ID, or still has DWARF or `.symtab`;
- an image has no matching symbol file, or more than one;
- a debuglink name or CRC does not match;
- an RSDS record holds a path instead of a bare PDB name;
- a symbol file sits in the runtime tree;
- a symbol file maps to no image.

`build-windows-shipping` runs it over the staged runtime/tools/samples
components and uploads the PDBs and manifest as the separate
`shipping-symbols-<sha>` artifact. Symbol-server hosting belongs to OPS-100.

CTests: `ShippingManifest_SymbolManifestTool` runs
`Tests/Tools/test_shipping_symbol_manifest.py`. It builds real gcc fixtures
through the production launcher, and clang/lld-link PE+PDB fixtures
cross-checked with `llvm-readobj` and `llvm-pdbutil`. It also checks that
`SPARK_SHIPPED_IMAGE_TARGETS` names every image target installed by an
`install(TARGETS)` rule in the root, `Spark*/`, `GameModules/` and `cmake/`
CMake files, and that no `CPACK_COMPONENTS_ALL` list (root or
`cmake/SparkCPackOptions.cmake`) names the symbols component.
`ShippingManifest_PrivateSymbols` is registered only in a `STRIP_DEBUG_SYMBOLS`
ELF tree with tests enabled; it installs the runtime/tools/samples and symbols
components to separate roots under `<build>/shipping-symbol-stage` and maps
every installed image, so it needs every installed target built. The local
linux-shipping evidence is with `ENABLE_LTO=OFF`; a full LTO (preset default)
build with `-g`, and its disk, memory and time on hosted runners, has not been
measured. MSVC PDB output under `/Brepro` and the hosted job have not yet been
observed.

### Build-output reproducibility (BLD-100)

`cmake/SparkReproducibleBuild.cmake` maps both roots out of optimized GCC/Clang
outputs: `-ffile-prefix-map=<source>/=` and then `-ffile-prefix-map=<build>=.`
(the later map wins, so it also covers a build tree inside the source tree).
Without the build-root map every DWARF `comp_dir` (each target's binary
directory) carried the build path, so builds made in different directories
differed in `.debug_info` and, through the GNU build-id, in the stripped image.
Under GCC LTO the LTRANS units compile at link time, so GCC also gets both maps
as link options, and every compile gets `-frandom-seed=<OBJECT>`: without a
seed GCC names the LTO IR sections of an object from the clock and pid, so even
two builds in one directory produced different static libraries.

Known limit: GCC's LTO IR (the members of static libraries such as
`libSparkAssetPipelineCore.a`) still records the build directory; no prefix map
rewrites it. Those members differ between trees in different directories,
although the linked images and `.debug` files are equivalent.

`tools/compare_build_outputs.py` compares builds with the standard library only:

```bash
python3 tools/compare_build_outputs.py manifest <root> --output a.json   # one tree
python3 tools/compare_build_outputs.py compare a.json b.json --report r.json
python3 tools/compare_build_outputs.py trees <root-a> <root-b>           # both at once
```

The `spark.build-output-manifest/1` manifest lists every ELF, PE and `ar` file
under the root: relative path, size, SHA-256, the identity (GNU build-id; COFF
timestamp, RSDS GUID/age/PDB name and whether the image carries the `/Brepro`
REPRO debug entry) and a SHA-256 per section or archive
member. It holds no absolute path, so equivalent trees give byte-identical
manifests. PDBs are not compared: the RSDS record in the image identifies them.
The comparison exits 1 on any missing, extra or differing output and names the
first differing section that is a cause (headers, the build-id note and the
debuglink CRC only follow other changes). A PE image linked without `/Brepro`
whose COFF timestamp differs is reported at `<pe-headers> (COFF timestamp)`:
there the timestamp is the link time and the debug directory that repeats it
is derived. Under `/Brepro` the timestamp is a content hash, so the content
section is named instead. An empty or malformed tree exits 2.

CTests (label `reproducibility`):

- `ReproducibleBuild_CompareTool` runs `Tests/Tools/test_compare_build_outputs.py`
  on gcc ELF, crafted PE and `ar` fixtures.
- `ReproducibleBuild_LinuxToolTargets` (ELF trees with objcopy; `RUN_SERIAL`,
  about 80 s) runs `compare_build_outputs.py two-tree`. It copies the source
  tree (no `.git`, no `build/`) to `<build>/reproducible-build-trees/a/src` and
  `.../tree-b/nested/src`, configures each with the linux-shipping settings
  (MinSizeRel, LTO, `STRIP_DEBUG_SYMBOLS=ON`; `SPARK_STRICT_DEPS` stays OFF
  because the copies have no `.git` for the third-party audit) and this tree's
  compiler, with no compiler launcher and no `CFLAGS`/`CXXFLAGS`/`LDFLAGS`,
  builds `SparkCooker`, and compares `bin/`: the stripped image and its
  `.debug`. Before the build-root map both differed (`.debug_info`, and the
  image's build-id). It has passed locally only with GCC 13.3. The inner
  build uses the defaults (`ENABLE_LTO=ON`, the compiler's default standard
  library), not the Clang lane's `ENABLE_LTO=OFF`/libc++ flags, and no hosted
  GCC 14 or Clang lane has run it yet; one manual Clang two-tree run outside
  the CTest was equivalent.

The `reproducibility-windows` job checks the repository out twice (`a` and
`tree-b/nested/src`), builds and installs the `windows-shipping` preset in each
with no compiler cache, and compares the two install trees. It is job-level
`continue-on-error` and not a `required-ci-gate` dependency until a hosted run
shows equivalent trees, and it has not run yet. MSVC objects embed CodeView
(`/Z7`) with absolute paths that `/d1trimfile` does not rewrite, so the static
libraries in the SDK install may differ between the trees; the job reports
that rather than hiding it.

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
- `clang-tidy` — Clang Debug static analysis; the job is blocking, while
  individual diagnostics are advisory
- `todo-count` — fails if TODO count exceeds threshold (20)
- `build-installer` — builds the `SparkInstaller` target
- `report-ci-errors` — aggregates `ci-errors-*` artifacts from failed jobs; findings from advisory lanes (job-level `continue-on-error`, e.g. `build-linux-msan`) are listed but do not fail the report. The reporter is loaded from the trusted `Working` checkout, so a change to it takes effect only after it lands there

To verify CI-100's required-job failure propagation on the `Working` ref, run
`gh workflow run build.yml --ref Working -f simulate_required_job_failure=true`. The manual-only input deliberately fails
the required `validate-ci-tools` job; `Required CI Gate` must then fail as well.
This is a red control run and cannot qualify a release commit.

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
  - Output filenames updated to match current CI (`asan-ubsan-lsan-results.txt`, etc.). Corrected 2026-09-24: CI's ASan evidence is the `run-sanitizer-tests.sh` directory (`junit.xml` + `metadata.json`), not a results `.txt` file.
  - Added the new `ci-linux-asan` / `ci-linux-tsan` presets as alternatives.
  - Noted MSan builds only the `SparkTests` target in CI; added `|| true` to match CI (2026-09-06: the recipe now builds the MSan-instrumented libc++ first and runs with `halt_on_error=1`, so the `|| true` was dropped again).
  - Added sccache/`continue-on-error` notes for the Windows jobs and the v145 VS 2026 variant.
  - Windows VS 2022 / VS 2026 recipes switched to Ninja Multi-Config + sccache (2026-09-06); the Visual Studio-generator configure now applies only to `build-windows-shipping`'s preset.
  - Added the jobs that did not exist in the source: `check-thirdparty-manifest`, `coverage`, `clang-tidy`, `todo-count`, `build-installer`, `report-ci-errors`, plus the macOS and MinGW-Wine reproduction recipes.
  - Noted the Linux GCC job uses gcc-14/g++-14.
  - 2026-09-25: added build-output reproducibility (BLD-100): the build-root prefix map, the GCC LTO seed, `tools/compare_build_outputs.py`, the `ReproducibleBuild_*` CTests and the advisory `reproducibility-windows` job, measured locally with GCC 13.3.

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
