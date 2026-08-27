---
name: sparkengine-validation-and-qa
description: >-
  SparkEngine test & QA runbook — how tests are registered, selected, and gated.
  TRIGGER when: adding or registering a test, running SparkTests / ctest, filtering to one
  test or file, a test "doesn't run" or "isn't found", flaky-test warnings, sanitizer
  (ASan/TSan/MSan) runs, coverage thresholds, package-smoke / installed-SDK validation,
  golden-image or screenshot evidence, or deciding which CI checks are required vs advisory.
  DO NOT TRIGGER when: fixing the build itself or CMake configure errors
  (use sparkengine-build-ci-and-dependencies), launching/operating built binaries
  (use sparkengine-run-package-and-release), debugging engine runtime behavior
  (use sparkengine-debugging-playbook), or researching why a past bug happened
  (use sparkengine-failure-archaeology).
---

# SparkEngine — Validation & QA

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

Runbook for the test suite, its gates, and the honest state of every quality check.
All paths are repo-relative; all claims verified 2026-08-23 against the working tree
of branch `claude/whole-nine-yards-20260823` (uncommitted changes ahead of the
`34ee7ab7`/`0e1fe7e7` commits).

**Sibling boundaries** — do not use this skill for:

| Task | Use instead |
|---|---|
| Build/configure failures, CMake feature flags (`ENABLE_*`, `BUILD_TESTS`) semantics | `sparkengine-build-ci-and-dependencies` |
| Launching/operating the editor/engine binaries | `sparkengine-run-package-and-release` |
| Diagnosing an engine bug a test surfaced | `sparkengine-debugging-playbook` |
| History of past regressions and their root causes | `sparkengine-failure-archaeology` |

---

## 1. The test framework (no gtest, no Catch2)

Tests use a **custom zero-dependency framework**: `Tests/TestFramework.h` (macros + registry)
and `Tests/TestMain.cpp` (runner). Everything compiles into **one executable, `SparkTests`**.

- `TEST(name)` — **one argument**, a bare identifier. There is no
  `TEST("name", "[tag]")` two-arg form and no `SPARK_TEST` / `SPARK_TEST_ASSERT` /
  `ASSERT_FLOAT_EQ` macro. Those are hallucinated conventions that caused 16 test files
  to silently never compile (see §8).
- `TEST_F(FixtureClass, testName)` — fixture must define `SetUp()` and `TearDown()`.
- Non-fatal asserts: `EXPECT_TRUE/FALSE/EQ/NE/GT/LT/GE/LE/NEAR/THROW/NO_THROW/STR_CONTAINS`.
- Fatal asserts: `ASSERT_TRUE/FALSE/EQ/NE` — record the failure and throw `TestAbort`,
  which the runner catches **per test** (aborts one test, not the process). Use `ASSERT_*`
  before any dereference that would crash on failure.
- Registration is an intrusive linked list built at static-init; no discovery step. A test
  runs **iff its .cpp is compiled into the `SparkTests` target** — which is exactly why
  registration hygiene (§2) matters.
- Crashes are caught (SEH filter on Windows, signal handlers elsewhere) and print
  `[ CRASH  ] <testname>` plus a symbolicated stack trace instead of silently killing the suite.

## 2. Test registration — the canonical home

`Tests/CMakeLists.txt` is the **single registry**. A new `Tests/TestFoo.cpp` must be added
to the `SparkTests` source list there or it never compiles and never runs.

Layout facts (verified):

- ~543 test source files across `Tests/Test*.cpp`, `Tests/harden/Test_*.cpp`, and
  `Tests/Integration/`. The registered runner reports ~6,170+ individual `TEST()` cases.
- `SparkTests` links `SparkEngineLib` and additionally **compiles selected production .cpp
  files directly** (SparkEditor scene/serializer/core sources, SparkDaemon services,
  `GameModules/SparkGameMMOFPS` persistence/account/validation sources, FPS `GameMode.cpp`).
  When your test needs a module .cpp that isn't in the engine lib, add that .cpp explicitly —
  the file has many precedents with comments.
- CTest sees **one test**: `add_test(NAME SparkEngineTests COMMAND SparkTests)`. So
  `ctest -R SparkEngineTests` runs the whole suite; per-test selection is done via
  SparkTests selectors (§3), not ctest.
- Feature gating: `SPARK_TEST_HAS_IMGUI / _NETWORKING / _PHYSICS / _VULKAN / _OPENGL / _MOBILE`
  compile definitions are set from CMake options; tests `#ifdef` on them. ImGui availability
  additionally pulls in the 7 game-module test source groups.
- LTO is force-disabled on `SparkTests` (ThinLTO + `--gc-sections` discarded Logger symbols).

### The registration guard

```bash
bash tools/check-test-registration.sh
# OK output: "check-test-registration: OK — all Tests/Test*.cpp are registered in CMakeLists.txt"
```

Verified behavior and **limits**:

- Globs **only top-level** `Tests/Test*.cpp`. Files under `Tests/harden/` and
  `Tests/Integration/` are NOT covered — a new file there can still silently drop out.
- Opt-out: a file containing the comment `// test-registration: ignore` is skipped.
- **Not wired into CI or `tools/validate-all.sh`** (verified: no workflow references it, and
  `validate-all.sh` does not call it). Running it is a manual, pre-commit discipline.
  Wiring it into CI/validate-all is an `open` improvement (see `HARDEN_FLEET_HANDOFF.md`).

## 3. Running and selecting tests (SparkTests selectors)

Binary locations: `build/<preset>/bin/SparkTests[.exe]` for preset builds
(e.g. `build/windows-debug/bin/SparkTests.exe`), `build/bin/SparkTests` for the plain
`cmake -B build` layout CI uses.

Environment-variable selectors (substring matches):

```bash
SPARK_TEST_NAME=NavMeshQuery_FindNearestPoint ./SparkTests   # filter by test name
SPARK_TEST_FILE=TestNavMesh.cpp ./SparkTests                 # filter by source file (__FILE__ substring)
SPARK_TEST_EXCLUDE=LoadTest_,Stress ./SparkTests             # comma-separated name excludes
SPARK_TEST_LIMIT=50 ./SparkTests                             # stop after N tests (bisection)
```

CLI flags (all verified in `TestMain.cpp`):

```
--output-file <path>   write full output to a file
--errors-only          file gets only failures + summary
--junit-xml <path>     JUnit XML for CI
--shuffle [seed]       randomize order (prints seed for reproduction)
--retries <N>          re-run failed tests up to N times
--warn-is-error        promote known-flaky warnings to hard failures
--list-warnings        print the flaky-pattern registry and exit
```

**Footgun — the empty-selection green run.** A filter that matches nothing runs 0 tests and
exits 0. Before believing a filtered pass, confirm the summary line shows the expected count
(`Tests: 1 passed, 0 failed, 1 total`). A check that stops checking looks identical to a
check that passes.

### Known-flaky registry

`Tests/TestWarnings.h` holds substring patterns (9 entries as of 2026-08-23: ScopedTimer,
FreezeDetector ×3, CollabEdit, LiveEditBridge, prediction-reconciliation integration,
`LoadTest_FullEngine_3000Frames`, …). Matching failures print `[ WARN ]` + a GitHub Actions
`::warning` annotation and **do not** fail the suite. To promote back to hard failure,
delete the entry; to audit locally, run with `--warn-is-error`.

## 4. Production-source vs mirror tests

Three distinct test styles coexist — know which one you're writing:

| Style | Naming | What it exercises |
|---|---|---|
| Real-class tests | `Test<X>Real.cpp` | The actual production class, linked from `SparkEngineLib` or a directly-compiled module .cpp. Preferred for new coverage. |
| Mirror / standalone-math tests | e.g. `TestTF*.cpp` (TERRAFRONT) | Header-only wire-protocol PODs and re-derived rule math; module .cpp sources deliberately NOT compiled in (documented in `Tests/CMakeLists.txt` comments). |
| Behavioral/mock tests | plain `Test<X>.cpp` | Logic via test-local scaffolding. |

A mirror test that drifts from the production source proves nothing about the engine.
When a mirror and a Real test disagree, the Real test is the truth; fix or delete the mirror.
If you need production behavior from a module DLL, follow the `TFDatabase.cpp` pattern:
compile the minimal-dependency .cpp straight into `SparkTests`.

## 5. Gates — focused vs full

**Focused loop (while iterating):**

```bash
cmake --build build/windows-debug --target SparkTests --parallel   # or your preset dir
SPARK_TEST_FILE=TestFoo.cpp ./build/windows-debug/bin/SparkTests.exe
```

**Full local gate (before commit/PR)** — matches CLAUDE.md pre-commit (these lines
assume the raw `cmake -B build` layout; on a preset tree substitute
`cmake --build --preset <name>` and `ctest --test-dir build/<name> -C Release`):

```bash
cmake --build build --config Release 2>&1 | tail -30
cd build && ctest --output-on-failure && cd ..
bash tools/check-test-registration.sh          # manual — CI will NOT catch orphans for you
tools/validate-all.sh --warn-only              # hygiene checks (does NOT include test registration)
```

**RED-proof discipline:** after "fixing" a test or registering a new one, prove it can fail —
temporarily break the assertion, watch `[ FAILED ]`, revert. A test that was never seen red
may be a no-op (the c412f05b files "passed" for months by not existing in the build).

## 6. Sanitizers

CI configures Debug + `-DBUILD_TESTS=ON` with GCC 14 / Clang and runs `./bin/SparkTests`
directly. Reproduce on Linux with the same flags (from `.github/workflows/build.yml`):

| Lane | Flags | Suppressions | Blocking? |
|---|---|---|---|
| ASan+UBSan+LSan | `-fsanitize=address,undefined -fno-omit-frame-pointer` | `Tests/lsan_suppressions.txt` (via `LSAN_OPTIONS=suppressions=…`) | **Yes** |
| TSan | `-fsanitize=thread -fno-omit-frame-pointer` | `Tests/tsan_suppressions.txt` (via `TSAN_OPTIONS`) | **Yes** |
| MSan | `-fsanitize=memory -fsanitize-memory-track-origins=2 -stdlib=libc++` + `Tests/msan_ignorelist.txt`, `-DENABLE_SDL2=OFF` | ignorelist file | **No** — `continue-on-error`, uninstrumented system libc++ gives inherent false positives; test step is `|| true`, findings live in the `sanitizer-report-msan` artifact |

Presets `ci-linux-asan` / `ci-linux-tsan` exist in `CMakePresets.json` for local repro.
Run options used by CI: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:print_stats=1:check_initialization_order=1`,
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0`.

## 7. Package smoke (installed-SDK validation)

`Tests/PackageSmoke/` is a **separate CMake project** that consumes the *installed* engine
via `find_package(SparkEngine REQUIRED)` and links `Spark::SparkEngineLib` — it proves the
exported package (headers under `<SparkEngine/...>`, targets, transitive deps like
SDL2/miniz export correctness) works outside the source tree. This section is the
**canonical reproduction recipe** — build-ci, assets, and run-package cross-link here.
Reproduce locally:

```bash
cmake --install build --config Release --prefix stage       # or -DCMAKE_INSTALL_PREFIX at configure
cmake -S Tests/PackageSmoke -B smoke-build -DSparkEngine_DIR="$PWD/stage/lib/cmake/SparkEngine"
cmake --build smoke-build --parallel
ctest --test-dir smoke-build --output-on-failure            # test name: SparkInstalledPackageSmoke
```

Runs in CI only in `release.yml` ("Publish Builds": nightly cron 04:00 UTC, `release/**`
branches, `v*` tags, manual dispatch) — **not** in the per-PR `build.yml`. A package-export
regression can therefore merge green and only surface in the nightly. When you touch
install/export CMake, run the smoke locally before pushing.

## 8. Archaeology: the 16 orphan tests (commit `c412f05b`, 2026-07-13)

16 substantial test files (76–469 lines) sat in `Tests/` but were never added to the
`SparkTests` target — they **silently never ran in CI**. Registering them required fixing
three hallucinated macro conventions (§1), and actually running them surfaced **three real
engine bugs**: `LODGenerator::Simplify` accidentally O(collapses × triCount) (>150 s → ~6.5 s),
`HitchDetector` masking severe hitches (spike inflated its own baseline), and
`FontSystem::LayoutText` emitting quads for zero-area glyphs. One test
(`TestAutoLODPerformance`) had an arithmetically unsatisfiable assertion.

Lessons this skill encodes: (a) an unregistered test is indistinguishable from a passing
test; (b) `tools/check-test-registration.sh` exists because of this — run it; (c) reject
non-`TestFramework.h` macro dialects in review on sight. Full narrative:
`git show c412f05b`, the block comment at the bottom of `Tests/CMakeLists.txt`, and the
chronological entry in `sparkengine-failure-archaeology` (Era 4), which owns the story.

## 9. Visual evidence

- **Golden images** — `SparkEngine/Source/Utils/GoldenImageTest.h` +
  `Tests/GoldenImages/` (committed baselines) and `Tests/Output/` (run-time captures/diffs,
  not committed). Files are **raw RGBA with a `.png` extension** (4-byte LE width, height,
  then `w*h*4` bytes) — do not open in an image viewer and conclude corruption. Tolerances:
  `perPixelThreshold` default 10, `tolerancePercent` default 0.5%. A missing baseline is
  recorded as "no baseline", **not a failure** — so a golden test with no committed
  reference proves nothing. Create baselines with `GoldenImageTestRunner::CaptureGolden`,
  commit the file, and explain visual changes in the PR description.
- **Screenshots** — `tools/capture-screenshots.sh` captures editor/console/tool shots on
  Linux via Xvfb + llvmpipe into `docs/screenshots/`. It validates each capture is >1000
  bytes (a killed process still "produces" a file). The `Screenshots/` and
  `TestScreenshots/` directories at repo root are untracked local scratch, not a system.

## 10. Required vs advisory — what CI actually enforces (build.yml, working tree 2026-08-23)

This section is the **canonical home** for required-vs-advisory gate truth; siblings
cross-link here rather than restating it.

**Blocking jobs** (failure blocks the PR): `check-format`, `validate-prompts`,
`check-thirdparty-manifest`, `build-linux-asan`, `build-linux-tsan`,
`build-windows-vs2022`, `build-linux-gcc`, `build-linux-clang`, `coverage` (job itself),
`clang-tidy` (see caveat), the annotation-count job (`to-do-count`, job id spelled
without the inner hyphen — job itself), `build-installer`.

**Advisory** (`continue-on-error: true`): `build-linux-msan`, `build-windows-vs2026`,
`build-linux-mingw-wine`, `build-macos`.

Verified fine print — checks whose advertised threshold is **not actually enforced**:

| Check | Advertised | Reality (verified in workflow source) |
|---|---|---|
| `coverage` per-subsystem thresholds | Job title says "per-subsystem thresholds"; the coverage-report script (repo `scripts/` dir) exits 1 below threshold (Core 40%, Utils 60%, Graphics 30%, …) | CI invokes it with `\|\| true` — a below-threshold subsystem shows ❌ in the PR comment but **never fails the job**. Test failures in this job are also tolerated (`\|\| true`) by design; `LoadTest_` is excluded via `SPARK_TEST_EXCLUDE`. |
| annotation-count job, threshold 20 | "threshold: 20" | Emits only `::warning` when the to-do/fix-me marker count > 20; **always exits 0**. |
| `clang-tidy` | static analysis of the codebase | **Blocking job** — no `continue-on-error` in `build.yml` (CLAUDE.md's CI table is stale on this point; the workflow wins). Inside the job, warnings are advisory (`--warnings-as-errors=""`) and only configure/compile failures fail it. Lints only the **first 30 sorted .cpp files** in `SparkEngine/Source`. |
| `build-windows-vs2022` tests | Debug+Release matrix | Tests run **only for Release** (`if: matrix.config == 'Release'`, via `ctest --parallel`). A Debug-only test regression will not run on Windows CI. |
| Test-registration guard | exists in `tools/` | Not called by any workflow or by `validate-all.sh` — manual only (§2). |

Load-bearing extra gates worth knowing: `build-linux-gcc` Release **greps the test log**
for `VulkanParity_ShaderCompilePath_Asserted` and `VulkanParity_D3D11MilestoneSnapshot` and
asserts `ENABLE_VULKAN:BOOL=ON` in the CMake cache — renaming those tests breaks CI.
`msvc.yml` (MSVC Code Analysis → SARIF) and `codeql.yml` run separately from build.yml.

`candidate` improvements (unproven, do not claim as done): wire
`check-test-registration.sh` into `validate-all.sh`/CI; extend its glob to `harden/` and
`Integration/`; enforce coverage thresholds by dropping the `|| true`.

## Provenance and maintenance

Verified 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `34ee7ab7`/`0e1fe7e7`)
by reading/running the sources below — not by a full-suite or CI run at this exact
tree. Re-verify:

```bash
git status --short && git log -1 --format=%h                  # note the exact tree you re-verify against
bash tools/check-test-registration.sh                         # guard still passes / exists
grep -n "add_test" Tests/CMakeLists.txt                       # still single CTest entry
grep -n "continue-on-error" .github/workflows/build.yml       # advisory-job set drift
grep -n "coverage-report.sh" .github/workflows/build.yml      # '|| true' still neuters thresholds?
grep -n "head -z -n 30" .github/workflows/build.yml           # clang-tidy still 30-file subset?
grep -c "pattern" Tests/TestWarnings.h                        # flaky-pattern count drift
grep -rn "PackageSmoke" .github/workflows/                    # smoke still release.yml-only?
git show c412f05b --stat                                      # orphan-test archaeology intact
```
