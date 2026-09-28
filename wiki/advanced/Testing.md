# Testing

SparkEngine includes a comprehensive test suite using a lightweight internal test framework with CTest integration. For current test file and test case counts, see the auto-generated inventory section on this page.

> **Release boundary:** Test inventory is not release certification. The blocked
> and uncertified `stable-v1` profile remains governed by the required gates and
> exact-SHA evidence in `docs/site/readiness.json`.

**Source:** `Tests/TestFramework.h`, `Tests/`

## Test Framework

The engine uses its own lightweight test framework (no external test library dependencies). The framework is defined entirely in `Tests/TestFramework.h` and uses a static registry pattern for automatic test discovery.

### Architecture

```
TestFramework.h     — Macros, test registration, assertion tracking
TestMain.cpp        — main() entry point, runs all registered tests
Test*.cpp           — Individual test files (auto-registered)
```

The framework uses three global counters to track test execution:

| Global Variable | Purpose |
|-----------------|---------|
| `g_assertionsPassed` | Total assertions that succeeded |
| `g_assertionsFailed` | Total assertions that failed |
| `g_currentTest` | Name of the currently executing test |

### Test Registration

Tests register themselves at static initialization time via the `TestRegistrar` struct. Each test case is stored in a global `vector<TestCase>` and executed by the main runner.

```cpp
struct TestCase
{
    std::string name;               // Human-readable test name
    std::string file;               // Source file path
    int line;                       // Line number of the TEST() macro
    std::function<void()> func;     // Test body function
};
```

### Test Macros

```cpp
#include "TestFramework.h"

TEST(MyTestName) {
    EXPECT_EQ(value, expected);        // Equality check
    EXPECT_NE(value, unexpected);      // Not-equal check
    EXPECT_TRUE(condition);            // Boolean check
    EXPECT_FALSE(condition);           // Negative boolean check
    EXPECT_NEAR(a, b, tolerance);      // Floating-point comparison
    EXPECT_GT(a, b);                   // Greater-than check
    EXPECT_LT(a, b);                   // Less-than check
    EXPECT_GE(a, b);                   // Greater-or-equal check
    EXPECT_LE(a, b);                   // Less-or-equal check
    EXPECT_THROW(expr, ExceptionType); // Expects a specific exception
    EXPECT_NO_THROW(expr);             // Expects no exception
    EXPECT_WARN_ONLY(expr, "reason");  // Waive ONE environment-sensitive assertion (counted as waived, never a pass)
    EXPECT_NO_CRASH("reason");         // Runner-semantics probes only (CI-110 validator rejects it elsewhere)
    SKIP_TEST("reason");               // Mandatory in a #else placeholder; the reason is classified in test-warning-waivers.json
}
```

### Assertion Macro Reference

| Macro | Condition | Failure Output |
|-------|-----------|----------------|
| `EXPECT_TRUE(expr)` | `expr` is true | "FAIL: expr was false" |
| `EXPECT_FALSE(expr)` | `expr` is false | "FAIL: expr was true" |
| `EXPECT_EQ(a, b)` | `a == b` | "FAIL: a == b (actual != expected)" |
| `EXPECT_NE(a, b)` | `a != b` | "FAIL: a != b (both value)" |
| `EXPECT_NEAR(a, b, t)` | `|a - b| <= t` | "FAIL: |a - b| <= t (diff)" |
| `EXPECT_GT(a, b)` | `a > b` | "FAIL: a > b (actual <= expected)" |
| `EXPECT_LT(a, b)` | `a < b` | "FAIL: a < b (actual >= expected)" |
| `EXPECT_GE(a, b)` | `a >= b` | "FAIL: a >= b (actual < expected)" |
| `EXPECT_LE(a, b)` | `a <= b` | "FAIL: a <= b (actual > expected)" |
| `EXPECT_THROW(expr, T)` | `expr` throws `T` | "FAIL: Expected T from expr" |
| `EXPECT_NO_THROW(expr)` | `expr` throws nothing | "FAIL: Unexpected exception from expr" |
| `EXPECT_WARN_ONLY(expr, reason)` | `expr` is true; a false result is recorded as a *waived* assertion, not a failure | "WARN: expr (reason)" |
| `EXPECT_NO_CRASH(reason)` | reaching this line | counted as a no-crash-only assertion |
| `SKIP_TEST(reason)` | -- | marks the test `[ SKIPPED ]` (runtime capability unavailable) |

All macros use `do { ... } while(0)` for safe use in if/else blocks. Failed assertions print the file, line, and values to `stderr` but do **not** abort the test -- all assertions in a test body are evaluated.

### Waivers, empty tests, and the JUnit shape

- `EXPECT_WARN_ONLY(expr, reason)` is the preferred way to waive a **single**
  environment-sensitive assertion. An entry in `Tests/TestWarnings.h` waives the
  **entire** matched test (its failures become "known flaky" and do not fail the
  run); prefer the per-assertion macro.
- A test that executes zero assertions is reported as `[ EMPTY ]`. This is a
  non-failing label by default; pass `--empty-is-error` to the runner to treat it
  as a failure. `EXPECT_NO_CRASH(reason)` makes a does-not-crash claim countable
  instead of hiding it behind `EXPECT_TRUE(true)`; the CI-110 validator (below)
  allows it only in the runner-semantics probes.
- `SKIP_TEST(reason)` is mandatory for a `#else` placeholder when a feature is
  compiled out; a placeholder that silently passes fabricates evidence.
- The runner summary now prints `Assertions: ... N waived, M no-crash-only` and
  `Empty: N test(s) executed zero assertions`.
- **JUnit shape:** a known-flaky (warned) test is no longer written as
  `<skipped>`. It is emitted as `<testcase>` with
  `<properties flaky/flaky-reason/waived-assertions/>` and a
  `<flakyFailure message="Known flaky: ...">` child, and `<testsuites>` /
  `<testsuite>` carry `flaky=` and `empty=` attributes. `<skipped>` now means
  only "runtime capability unavailable"; consumers that treated it as such were
  previously also counting tolerated real failures.
- `SPARK_TESTS_WARN_IS_ERROR` (CMake option, default OFF) promotes warned
  assertions to failures in both ctest lanes; it stays OFF because
  `Tests/TestWarnings.h` still carries waivers whose comments document real
  non-determinism.
- Every whole-test waiver is cross-checked by the blocking CI-110 command
  `python Tools/validate_test_warnings.py` against
  `Tests/test-warning-waivers.json`. The metadata must have exact pattern
  parity, a named owner, and a future `YYYY-MM-DD` expiry; missing, duplicate,
  unowned, or expired entries fail the workflow. Prefer `EXPECT_WARN_ONLY` for
  a single environment-sensitive assertion so unrelated assertions remain
  strict.
- Per-assertion `EXPECT_WARN_ONLY` waivers are held to the same rule. The
  validator inventories every call site in `Tests/**/*.cpp` (comments and
  string literals are ignored) and attributes it to its enclosing `TEST` /
  `TEST_F` body (fixture tests are keyed `Fixture.Name`). Each (file, test)
  pair needs a schema-2 `assertionWaivers` entry with the exact number of
  `sites`, a named `owner`, and a future `expires` date. Unregistered, stale,
  expired, ownerless, miscounted, and out-of-test-body sites fail the workflow.
  The only exempt sites are the `RunnerSemanticsReal_*` probes in
  `Tests/TestRunnerSemanticsReal.cpp`, which exercise the macro itself.
  Schema-1 metadata (whole-test waivers only) is still accepted and declares
  no per-assertion waivers.
- The same validator owns the other two exception shapes (schema 3):
  - `EXPECT_NO_CRASH` is rejected anywhere in `Tests/**/*.{cpp,h}` except the
    `RunnerSemanticsReal_*` probes. A test must assert observable state; when
    nothing is observable, the test has no claim to make and is deleted.
  - Every `SKIP_TEST` (and the Vulkan `SkipOrFail` forwarder) is classified. A
    string-literal reason must start with exactly one `skipReasons` prefix; a
    reason built at run time needs a `dynamicSkips` entry keyed by file and
    test (`"test": null` for a helper outside any test body) with its exact
    site count. Each entry names an `owner` and a `kind`: `environment` (a
    missing platform capability; no expiry), `flaky` (timing or test-order
    coupling; needs a future `expires`), or `probe` (the skip is the behaviour
    under test). Unclassified, ambiguous, stale, miscounted, ownerless, and
    unexpiring flaky entries fail the workflow.

### Production-source census

`Tools/test_source_census.py` classifies every registered test as
production-source (executes shipped code), mirror (a test-local reimplementation
that cannot detect a regression in shipped code), or process-smoke. `--json <path>`
writes the census; `--check` fails on a new mirror test file or a `*_Skipped` test
that fabricates a pass. Its `MIRROR_BASELINE` is a shrink-only list -- a file
leaving it is an advisory, never a build failure. When quoting the suite size,
quote `productionSourceTests` alongside the total (measured on the 2026-09-05
sweep working tree: 7,233 registered `TEST`s, 4,965 production-source in 411
files, 2,268 mirror in 190 files, 108 `EXPECT_TRUE(true)` occurrences -- regenerate
with the census rather than copying these numbers): the raw pass count alone is
not the honest headline, because the remainder includes ~108 `EXPECT_TRUE(true)`
assertions (`tautologicalAssertions` in the JSON) and the zero-assertion tests now
surfaced as `[ EMPTY ]`. The CI ratchet (`.github/test-count-ratchet.json`) bounds
`minimumProductionSourceTests` (4900) and `maximumEmpty` (25, a first ceiling that
must be re-measured from the first Build run and ratcheted down).

`--profile-selectors [CTEST_JSON]` (RDY-010) guards release-profile evidence.
It takes every CTest labeled `stable-v1` or `module-profile` that runs
SparkTests and resolves its `SPARK_TEST_NAME` / `SPARK_TEST_FILE` /
`SPARK_TEST_EXCLUDE` filters to the `TEST`/`TEST_F` definitions they reach. It
uses the same `strstr` semantics as `TestMain.cpp`. A file filter is matched
against every spelling `__FILE__` can have, including absolute and backslashed
paths. The check fails if a selector:

- reaches a mirror file or a body that is only `EXPECT_TRUE(true)` /
  `EXPECT_NO_CRASH`
- runs the whole suite unfiltered
- has no valid `SPARK_TEST_EXPECT_COUNT`, or a count larger than the
  definitions it can see
- cannot be resolved to a literal

It always checks `Tests/CMakeLists.txt` statically, so Windows-only
registrations are covered on every host. When given `ctest --show-only=json-v1`
output, it checks that configured tree too. The Windows Release census step
runs both, and CTest `TestSourceCensus_ProfileSelectors`
(`Tests/Tools/test_source_census_profile.py`) runs both plus the mutations that
must fail.

## Running Tests

### Build with Tests Enabled

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build --config Release
```

### Run All Tests

```bash
# Via CTest (recommended). SparkTests is registered as two ctest lanes:
#   SparkEngineTests      -- everything except LoadTest_ / DeepStress_
#   SparkEngineLoadTests  -- labels "load;slow", SPARK_TEST_EXPECT_COUNT=22
# Budgets are configuration-dependent (Debug 900 s; optimized 240 s / 300 s for the load lane).
# A heavy family that already has its own pinned prefix lane is also excluded from
# SparkEngineTests via spark_exclude_from_main_suite() in Tests/CMakeLists.txt, and
# only under the condition that registers that lane: FPSLAN_ (FPSLANTwoClientConvergence),
# RacingCompleteRace_ (ModuleManifest_SparkGameRacing_RacingCompleteRace), TFScram_
# (TFScramAuth, always registered), and on Linux
# OpenGLGolden_ / VulkanGolden_RHI230_. Configure fails if a TEST name contains an
# excluded prefix without starting with it, since no lane would run that test.
# Run the full ctest set (not just -R SparkEngineTests) to execute every family.
ctest --test-dir build -C Release --output-on-failure --no-tests=error

# Via direct binary execution
./build/bin/Release/SparkTests  # VS/Ninja Multi-Config
# ./build/bin/SparkTests        # Single-config generator
```

### CI Test Evidence

The primary Windows, Linux, and macOS jobs retain the runner's granular JUnit
XML plus a JSON summary containing executed, passed, failed, errored, skipped,
duration, and slowest-test fields. The summary parser fails closed when the
report is missing, empty, malformed, contains a failure, or records fewer than
the expected registration floor. This prevents an executable launch failure
from being reported as a zero-failure test run. Repository badges count source
test definitions separately because platform and feature gates affect the
runtime set.

**CTest registration policy (CI-110).** Every `add_test` must carry a positive
`TIMEOUT` (at most 3600 s) and non-empty `LABELS` set in the same file: the
project never includes `CTest.cmake`, so an unbounded test that hangs stalls the
whole run instead of failing, and an unlabelled one is invisible to
`ctest -L unit|integration|process`. Label product registrations with the
product (`console`, `server`, `sparkbuild`, ...) plus `unit`, `integration`, or
`process`; the validator enforces only a non-empty set, so that category is a
review convention (fuzz and policy registrations keep `fuzz`/`security`). `python3 Tools/validate_ctest_policy.py` (no arguments) checks every
git-tracked first-party `CMakeLists.txt` and `*.cmake` that calls `add_test`,
outside `ThirdParty/`. Because loops, functions, and platform branches are only
resolved by CMake, `build-linux-gcc` and `build-windows-vs2022` also run
`ctest --show-only=json-v1` on the configured tree and pass it to
`validate_ctest_policy.py --ctest-json`. That step fails the job on any
violation or an empty inventory. To reproduce locally:

```bash
ctest --test-dir build/linux-gcc-release --show-only=json-v1 > ctest.json
python3 Tools/validate_ctest_policy.py --ctest-json ctest.json --build-dir build/linux-gcc-release
```

The configured-tree view also enforces the CI-110 shipped-binary rule, which is
why `--ctest-json` requires `--build-dir <configured tree>`. The shipped set is
every executable target with an install rule in that tree's CMake file-API
codemodel. The root `CMakeLists.txt` requests the codemodel with
`cmake_file_api()` whenever `BUILD_TESTS` is on, which needs CMake 3.27 or
newer. A missing reply, or a tree that ships nothing, fails with exit 2. Because
the shipped set is read from the build rather than from the tests, a shipped
binary that no registered test names fails with "built in this tree but no
registered test runs it". Each shipped binary needs at least one test that names
it (as the command, an argument, a `-D...=` value or an `ENVIRONMENT` value),
does more than print `--help`/`--version` (a `-DSPARK_VERSION_EXECUTABLE=`
runner counts as a version probe), and carries an `integration`, `smoke` or
`process` label. `SparkInstallerHeadlessSmoke` is the installer's lane: it runs
a headless install, resume and refusal against a local fixture repository.
`KNOWN_BINARY_LANE_GAPS` lists the documented exceptions. Each one is scoped to
the configured `CMAKE_SYSTEM_NAME` values it covers, and every run prints the
applicable gaps as notes:

- `SparkLauncher` (every platform) is a GUI with no headless mode. Only its
  launch-request logic is tested, in process.
- `SparkShaderCompiler` (Linux and macOS) is built and installed there, but
  `d3dcompiler_47` is its only integrated backend, so every compile is refused.
  Its lanes run on Windows only.

An entry whose binary gains a lane on a gap platform fails until it is removed.
The first form of the rule only checked binaries that some test already named.
On a 2026-09-27 Windows Release tree, before the installer lane existed, it
flagged exactly `SparkInstaller`.

`cmake/RunSparkTests.cmake` **requires** `-DSPARK_TEST_TIMEOUT_SECONDS=<n>`; any
script that invokes it directly must pass one (the 180 s default is gone so no
configuration can inherit the fast configuration's wall clock).

**Documented selectors resolve in a real tree.** `validate.py` proves each
work-item `-L`/`-R` filter names a registered label or test.
`tools/site-data/check_documented_selectors.py` checks the same commands, plus
every `ctest` line in a fenced block on this page, against a configured tree. A
command applies when its `--test-dir` (or `--preset`) resolves through
`CMakePresets.json` to the preset tree with the same name as `--build-dir`.
Every other command is reported as not applicable and is never counted. For each
applicable command it runs `ctest --show-only=json-v1` with the command's own
selection flags. The selection must hold at least one test without `DISABLED`,
and every enabled selected test's executable must exist. A filter declared in
`plannedTestSelectors` is listed as debt, not as a pass. It exits 2 when the tree
is not configured or no command applies, so it cannot stop checking and still
pass. CTest runs it as `DocumentedTestCommands_SelectBuiltTests` in `build/<preset>`
trees, where exit 2 reports Skipped. `DocumentedTestCommands_CheckerFailsClosed`
runs its fixture tests. The full-CTest lanes then run what these selections
name. The `SPARK_TEST_*` environment selectors of `SparkTests` are not ctest
commands and are not covered by this check.

```bash
python3 tools/site-data/check_documented_selectors.py --build-dir build/linux-gcc-release
python3 tools/site-data/check_documented_selectors.py --build-dir build/windows-release --config Release
```

**Test-count ratchet.** `.github/test-count-ratchet.json` carries a `baseline`
block measured at `4fec0297` (Linux lanes 6917 recorded / 6914 executed / 3
skipped; `windows-vs2022-release` 6819 / 6818 / 1) and per-lane floors of 6900
(Linux) and 6800 (Windows), replacing the old 6600/5000 wording. Lane keys must
stay equal to the aggregate step's `--expected-lane` list
(`test-workflow-failure-propagation.py` asserts the two sets are equal). The
Windows Debug SparkTests run is still guarded off in `build.yml` until the
per-config timeout is measured on the runner.

### SEC-120 Fuzz-Policy Checks

The blocking `fuzz-policy` Linux job runs the standalone policy CMake project.
Its structural gate and Python adversarial tests do not substitute for production
fuzz-harness execution. The job also runs bounded `json-utils`, `neural-weights-nnw`,
and `crash-manifest-parser` sanitizer smokes/reviewed-seed replays, but SEC-120 remains
release-blocking while 102 inventoried parsers, 151 deferred candidates, and scheduled
mutation-campaign evidence remain.
See [Fuzz Policy and Parser Security](Fuzz-Policy-and-Parser-Security.md).

```bash
CC=clang CXX=clang++ CXXFLAGS="-stdlib=libstdc++" \
  LDFLAGS="-stdlib=libstdc++" \
  cmake -S tools/fuzz-policy -B build/fuzz-policy
cmake --build build/fuzz-policy --target check-fuzz-policy
cmake --build build/fuzz-policy --target SparkFuzzJsonUtils SparkFuzzNeuralWeights SparkFuzzCrashManifest
# -C is required by multi-config generators (Visual Studio) and ignored by
# single-config ones; without it CTest reports "Not Run" on Windows.
ctest --test-dir build/fuzz-policy --output-on-failure --no-tests=error -C Release
ctest --test-dir build/fuzz-policy --output-on-failure -L '^fuzz$' --no-tests=error -C Release
```

The registered checks are `FuzzPolicy` (the structural gate),
`FuzzPolicyAdversarial` (the hostile regression suite), and the three production fuzz
smokes. The policy checks register only when
`SPARK_ENABLE_FUZZ_POLICY_CHECKS` is on, which defaults to `BUILD_TESTS`, so an
engine-only configure does not require Python. Release publication additionally runs
`check_fuzz_policy.py --require-closure`, which fails today by design.

For a multi-config generator such as Visual Studio, replace `Release` with the
configuration being tested.

### Run Registered CTest Entries

```bash
# CTest patterns match registered CTest entries, not individual source-test
# file names. This entry runs the native SparkTests aggregate.
ctest --test-dir build -C Release -R "^SparkEngineTests$" --output-on-failure --no-tests=error

# The load/stress family runs in its own lane, excluded from SparkEngineTests.
ctest --test-dir build -C Release -R "^SparkEngineLoadTests$" --output-on-failure --no-tests=error

# An independently registered, targeted CTest entry.
ctest --test-dir build -C Release -R "^SparkSaveCompatibilityTests$" --output-on-failure --no-tests=error

# Installed-SDK template tests: the -D and the label selector are BOTH required.
# Without the -D the label selects zero tests and ctest exits 0 - a check that stopped checking.
cmake -B build -DSPARK_ENABLE_INSTALLED_SDK_TESTS=ON
ctest --test-dir build -L installed-sdk --no-tests=error

# Linux installed-package consumer (ASSET-220; local, non-hosted evidence). Installs the
# complete build into build/package-consumer-linux/<config>/prefix, builds and ctests
# Tests/PackageSmoke against only that prefix, and fails if the consumer resolved any
# header or library from the source or build tree. Needs a full build, libgl-dev and
# `make`: the consumer always uses the Unix Makefiles generator (whatever the engine
# tree uses) because the boundary proof reads its depfiles and link.txt. An empty
# build type (single-config tree without CMAKE_BUILD_TYPE) runs the consumer as Release.
cmake -B build -DSPARK_ENABLE_PACKAGE_CONSUMER_TESTS=ON
ctest --test-dir build -L package-consumer-linux --no-tests=error --output-on-failure

# Ten-minute headless FPS/NullRHI soak (HEAD-220; Linux, local evidence). Samples VmRSS,
# applies the provisional leak-slope ceiling and requires zero NullRHI resources live at
# device shutdown (a bridge/device teardown guard: modules cannot reach that device).
# Anchor the label: a bare -L soak also selects nullrhi-soak/server-soak.
cmake -B build -DSPARK_ENABLE_SOAK_TESTS=ON
ctest --test-dir build -L '^soak$' --no-tests=error --output-on-failure

# Installed experimental-module objective runs (MOD-330/340/350/370/380; Windows or Linux
# headless, local evidence). Tests/PackageSmoke/RunInstalledModuleObjective.cmake installs
# the runtime and samples components, then drives the installed SparkEngine and module on
# NullRHI through Tests/PackageSmoke/ModuleObjectives/<Module>.cmake: every scripted
# command must dispatch ok, every rule must match that command's own audit output, and
# restart phases must reproduce an earlier process's output byte for byte, and no output may
# name the source or build tree. Real-time runs of several minutes each.
# ModulePackageObjectiveParserContract (parser cases plus a lint of every spec) is always
# registered, and so is the short SparkGameShowcase_PackagedSmoke on Linux headless (MOD-300).
cmake -B build -DSPARK_ENABLE_MODULE_PACKAGE_RUNS=ON
ctest --test-dir build -C Release -L module-package-run --no-tests=error --output-on-failure

# TERRAFRONT dedicated server + two headless clients (TF-110; local evidence until a
# dedicated CI job exists). Needs SparkEngine and SparkGameMMOFPS; timing-sensitive,
# so it is kept out of the required full-ctest lanes. TerrafrontMultiClient_Harness
# (parser/comparator/verdict unit tests) is always registered. One process run per
# scenario: OnboardSpawnMove, CombatKillRespawn, ForgedStateRejectedAndAudited,
# ReconnectRestoresAllowedState, TerritoryReplicates, VehicleLifecycle. The server's
# script repeats its faction-addressed harness verbs (tf_place_faction, tf_flux_floor,
# tf_damage_vehicles, tf_capture) across windows sized for a client clock that starts
# 0.5-10.5 s after the server's; a run outside that lag fails and says so.
cmake -B build -DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON
ctest --test-dir build -L terrafront-multiclient --no-tests=error --output-on-failure
ctest --test-dir build -R '^TerrafrontMultiClient_VehicleLifecycle$' --no-tests=error --output-on-failure

# Verbose output
ctest --test-dir build -C Release -V --no-tests=error

# List all available tests without running them
ctest --test-dir build -C Release -N --no-tests=error
```

`TestPhysics`, `TestECS`, and `TestAnimation` are source-test families, not
CTest registration names. Do not put those strings in a CTest `-R` selector; an anchored
registered name plus `--no-tests=error` prevents a zero-selected CTest run from
being reported as success. The executed-test population is the **union** of
`SparkEngineTests` and `SparkEngineLoadTests`, so a summary quoting one
`SparkTests-junit.xml` is quoting half the run. Note also that
`SPARK_ENABLE_INSTALLED_SDK_TESTS` only controls the local CTest registration: the underlying check
is not opt-in, because `release.yml` runs `cmake -P cmake/VerifyInstalledTemplates.cmake` on every
published Windows and Linux package. For an interactive native-runner filter, use
`SPARK_TEST_FILE` or `SPARK_TEST_NAME` and enforce a nonzero runner total:

```powershell
$runner = '.\build\bin\Release\SparkTests.exe' # Adjust to the configured output path.
$env:SPARK_TEST_FILE = 'TestPhysicsComponents.cpp' # Or use SPARK_TEST_NAME='Physics'.
$output = & $runner --quiet 2>&1
$exitCode = $LASTEXITCODE
$summary = $output -join "`n"
$output
Remove-Item Env:SPARK_TEST_FILE
if ($exitCode -ne 0 -or $summary -notmatch 'Tests:\s+.*,\s+[1-9]\d* total') {
    throw 'SparkTests failed or the filter selected zero tests.'
}
```

### Run Tests in Parallel

```bash
ctest --test-dir build -C Release --output-on-failure --no-tests=error -j$(nproc)
```

### Run Windows Tests Under Wine (Cross-Compilation)

Cross-compile with MinGW and run the exact same Windows D3D11 code paths under Wine on Linux:

```bash
# Build Windows .exe
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --parallel $(nproc)

# Run the full SparkTests suite under Wine
tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

# Or run the full automated test suite (unit tests + live engine + stress + break tests)
python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release
```

Results vary by branch and platform image. See [Cross-Compilation: Wine Testing](../platform/Cross-Compilation-Wine-Testing.md) for full setup and troubleshooting.

### Engine/Editor Test Mode Flags

Both the engine and editor support `--test-frames N` for automated testing:

```bash
# Engine: run 60 frames then exit
./SparkEngine -test-frames 60                              # Linux
wine64 SparkEngine.exe -test-frames 60                     # Wine

# Editor: skip project browser and run 120 frames
./SparkEditor --test-mode --test-frames 120                # Linux
wine64 SparkEditor.exe --test-mode --test-frames 120       # Wine
```

#### Scripted console timelines (`-exec`, `-exec-audit`, `-test-seconds`)

The engine can replay a console timeline on Windows (windowed and headless) and
on Linux (headless and SDL2 windowed). The shared implementation is
`SparkEngine/Source/Core/ExecScript.{h,cpp}`:

```bash
./SparkEngine -headless -game libSparkGameMMOFPS.so -require-game \
    -exec server.cfg -exec-audit server-audit.log -test-seconds 60
```

- Script lines are `<frame> <command>` or `t<seconds> <command>`. A line with
  no prefix runs at frame 0, `#` starts a comment, and CRLF files are accepted.
  Entries that share a due time run in file order. A late frame catches up every
  due entry once, including repeated commands; playback never drops or coalesces them.
- `-test-seconds N` exits after N wall-clock seconds. The clock starts at the
  first main-loop tick, so boot time is not counted.
- By default every executed command is appended to `exec_audit.log` in the
  working directory, which is the file the package smokes read. Use `-exec-audit <path>`
  to give each process its own file when several are launched from the same
  directory. A relative path is resolved against the launch directory.
- Console markers are `[exec] frame N (t=X.Xs, entry=I): command`, where `I` is
  the zero-based index in the parsed, due-ordered schedule, reset on each load.
  Audit headers retain their existing format. Each block includes recent console
  history, so earlier markers can recur before its own marker. The installed
  module objective parser requires exactly one marker matching the block's
  schedule index, frame, elapsed time and command, and requires all scripted
  commands in order. This distinguishes catch-up from duplicate or missing execution
  even when commands, scheduled times or rounded execution times coincide.
- Sensitive console commands, the ones registered with `RegisterSensitiveCommand`
  (for example `tf_register` and `tf_login`), are written as
  `<name> <arguments-redacted>` in both the `[exec]` console line and the audit
  file. The redaction goes through `SimpleConsole::RedactSensitiveArguments`.
  It fails closed: a command that is neither registered nor a CVar (for example
  `tf_login` when its module did not load) is also written as
  `<name> <arguments-redacted>` when it has arguments.
- On Linux, a `-test-seconds` value that is not a positive finite number
  (including `nan`, `inf`, `0` and negatives), or an `-exec` script that cannot
  be read, fails the launch with a non-zero exit code. A Linux build without
  SDL2 also rejects `-exec` and `-test-seconds` unless `-headless` is given,
  because its no-window fallback runs a fixed ten ticks and never plays a timeline.

Coverage: `Tests/TestExecScript.cpp` (`SPARK_TEST_FILE=TestExecScript.cpp`).

## Test Categories and Coverage

The test files cover all major engine subsystems:

### Core & Utilities

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestMathUtils` | 11 | Math utility functions, vector operations |
| `TestObjectPool` | 6 | Generic object pool allocation/deallocation |
| `TestRingBuffer` | 14 | Circular buffer operations, wrap-around |
| `TestResult` | 8 | Result/Error type handling |
| `TestStringUtils` | 19 | String manipulation, parsing, formatting |
| `TestColorUtils` | 18 | Color conversion (RGB, HSL, hex) |
| `TestFileUtils` | 15 | File path operations, extension parsing |
| `TestUUID` | 12 | UUID generation and comparison |
| `TestRandomEngine` | 11 | Random number generation, seeding |
| `TestBitFlags` | 14 | Bitwise flag operations |
| `TestFrameAllocator` | 8 | Per-frame linear allocator |
| `TestScopedTimer` | 3 | High-resolution timer scoping |
| `TestThreadSafeQueue` | 10 | Thread-safe queue operations |
| `TestLocalFileCache` | 15 | File caching system |
| `TestConfigParser` | 16 | INI/config file parsing |
| `TestDeltaSmoother` | 10 | Frame delta time smoothing |

### ECS (Entity Component System)

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestECSWorld` | 11 | Entity creation, component add/remove, queries |
| `TestECSIntegration` | 9 | System integration with World |
| `TestFPSComponentsReal` | 11 | Shipped Decal, Projectile, Interaction components |
| `TestSprite2DComponents` | 35 | 2D sprite rendering and animation |
| `TestPhysicsComponents` | 22 | RigidBody, Collider component validation |

### Physics

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestPhysicsComponents` | 22 | Physics component creation and validation |
| `TestFrustumCulling` | 11 | View frustum culling accuracy |

### AI & Navigation

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestAIBehaviorTree` | 16 | Behavior tree node execution, composites |
| `TestNavMesh` | 11 | NavMesh pathfinding, A* search |
| `TestSteeringBehaviors` | 15 | Steering: seek, flee, arrive, wander |
| `TestEnvironmentQuery` | 12 | EQS spatial queries |

### Animation

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestAnimationSystem` | 17 | State machines, blending, IK, evaluation |
| `TestAnimationRetargeting` | 9 | Skeleton retargeting between different rigs |
| `TestClothSimulation` | 4 | Cloth physics simulation |

### Networking

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestNetBuffer` | 29 | Network buffer serialization/deserialization |
| `TestNetworkEncryption` | 17 | Standalone mirror of legacy XOR/FNV prototype behavior; does not execute production networking and is not security evidence |
| `TestClientPrediction` | 5 | Client-side prediction and reconciliation |
| `TestDedicatedServer` | 27 | Server lifecycle, RCON, map rotation |

### Gameplay Systems

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestWeaponSystem` | 18 | Fire modes, reload, recoil, ADS |
| `TestWeaponMechanicsReal` | 22 | Shipped WeaponSystem fire, cooldown, reload, recoil, spread, ADS, switching |
| `TestInventorySystem` | 11 | Item add/remove, stacking, weight |
| `TestQuestSystem` | 10 | Quest stages, objectives, completion |
| `TestGameModeReal` | 11 | Shipped PlayerScore K/D, rules, spawn points, presets |
| `TestAchievementSystem` | 5 | Achievement tracking and unlocking |
| `TestDestructionSystem` | 5 | Object destruction and debris |
| `TestCooldown` | 14 | Cooldown timer management |

### Events & Systems

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestEventSystem` | 10 | Pub/sub, subscribe, unsubscribe, publish |
| `TestCoroutineScheduler` | 10 | Coroutine scheduling, yield, resume |
| `TestTween` | 14 | Easing functions, value interpolation |

### Engine Context & Infrastructure

| Test File | Cases | Description |
|-----------|-------|-------------|
| `harden/Test_tests_enginecontext_real` | 2 | Shipped EngineContext registry: unregister, re-register |
| `TestCommandHistory` | 10 | Console command history and recall |
| `TestDebugTools` | 31 | Debug visualization, imgui panels |
| `TestPlayModeManager` | 33 | Play/pause/stop mode transitions |
| `TestInputSystem` | 11 | Input state tracking and mapping |
| `TestInputBindings` | 5 | Configurable key bindings |
| `TestChromeTracing` | 5 | Chrome trace output format |
| `TestPerformanceStats` | 10 | FPS, frame time, memory tracking |

### Graphics & Post-Processing

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestFogSystem` | 17 | Fog calculation (linear, exponential) |
| `TestScreenSpaceEffects` | 16 | SSAO, SSR parameter validation |
| `TestPostProcessingPipeline` | 11 | Effect chain ordering and configuration |
| `TestTemporalEffects` | 11 | TAA settings and jitter patterns |
| `TestMeshLOD` | 8 | LOD distance switching |
| `TestLightManager` | 13 | Light creation, shadow setup |
| `TestUpscalingSystem` | 5 | Resolution upscaling parameters |
| `TestNoiseGenerator` | 7 | Perlin/simplex noise output ranges |
| `TestSplatmapSystem` | 10 | Terrain texture splatmaps |

### Scene & Save

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestSceneSnapshotSerializer` | 19 | Scene snapshot save/load round-trip |
| `TestSaveSystemRoundTripReal` | 17 | Shipped SaveSystem round trips, metadata, slots, recovery |
| `TestLoadingScreen` | 4 | Loading screen state management |

### World Systems

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestWeatherSystem` | 8 | Weather transitions, intensity |
| `TestDayNightCycle` | 10 | Time progression, sunrise/sunset |
| `TestSequencer` | 10 | Cinematic timeline tracks and playback |

### Other

| Test File | Cases | Description |
|-----------|-------|-------------|
| `TestDialogueSystem` | 4 | Dialogue tree navigation |
| `TestLocalizationSystem` | 6 | String localization and language switching |
| `TestReplaySystem` | 4 | Game replay recording/playback |
| `TestUISystem` | 6 | UI layout and event handling |
| `TestVisualScriptSystem` | 0 | Placeholder for visual scripting tests |

## Adding a New Test

1. Create a test file in `Tests/`:

```cpp
// Tests/TestMyFeature.cpp
#include "TestFramework.h"
#include "MyFeature.h"

TEST(MyFeature_BasicTest) {
    MyFeature feature;
    feature.Initialize();
    EXPECT_TRUE(feature.IsReady());
}

TEST(MyFeature_EdgeCase) {
    MyFeature feature;
    EXPECT_EQ(feature.Compute(0), 0);
    EXPECT_NEAR(feature.Compute(1.0f), 1.0f, 0.001f);
}

TEST(MyFeature_ExceptionHandling) {
    MyFeature feature;
    EXPECT_THROW(feature.InvalidOp(), std::runtime_error);
    EXPECT_NO_THROW(feature.SafeOp());
}

TEST(MyFeature_Comparisons) {
    MyFeature feature;
    EXPECT_GT(feature.GetSize(), 0);
    EXPECT_LE(feature.GetLoad(), 1.0f);
}
```

2. Add the source file to the explicit test-source list in `Tests/CMakeLists.txt`. CI runs `tools/check-test-registration.sh` and fails when a test source is present but not registered. See [Build System and CMake Modules](Build-System-and-CMake-Modules.md) for build configuration.

3. Build and run:

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

### Test Naming Conventions

Follow these conventions for consistency:

- Test file: `TestFeatureName.cpp` (e.g., `TestPhysicsComponents.cpp`)
- Test case: `FeatureName_DescriptiveAction` (e.g., `PhysicsComponents_CreateDynamicBody`)
- Use underscores to separate the feature from the behavior being tested

### Testing Best Practices

1. **Keep tests independent** -- Each `TEST()` should set up its own state and not depend on other tests
2. **Test edge cases** -- Zero values, empty collections, maximum values
3. **Use `EXPECT_NEAR` for floating-point** -- Never use `EXPECT_EQ` for float comparisons
4. **No external dependencies** -- Tests should run without network, filesystem, or GPU access
5. **Fast execution** -- Individual tests should complete in milliseconds

## CI Integration

Tests run automatically on every push via GitHub Actions. The CI matrix covers multiple platforms, compilers, and configurations.

### CI Build Matrix

| Job | Runner | Compiler | Configs | Key Flags |
|-----|--------|----------|---------|-----------|
| `check-format` | ubuntu-24.04 | clang-format | -- | `--dry-run --Werror` |
| `validate-prompts` | ubuntu-24.04 | -- | -- | `--ci` |
| `build-linux-gcc` | ubuntu-24.04 | GCC | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-clang` | ubuntu-24.04 | Clang | Debug, Release | `-DBUILD_TESTS=ON` |
| `build-linux-asan` | ubuntu-24.04 | GCC | Debug | ASan + UBSan + LSan |
| `build-linux-tsan` | ubuntu-24.04 | GCC | Debug | TSan (thread races) |
| `build-linux-msan` | ubuntu-24.04 | Clang + MSan-instrumented libc++ 18.1.3 (built in-job, cached) | Debug | MSan + ignorelist, `-DENABLE_VULKAN=OFF`, `continue-on-error` |
| `build-windows-vs2022` | windows-2022 | MSVC v143 | Debug, Release | Ninja Multi-Config + sccache, `-DBUILD_TESTS=ON -DBUILD_GAME_MODULES=ON` |
| `build-windows-vs2026` | windows-2025-vs2026 | MSVC v145 | Debug, Release | Ninja Multi-Config + sccache, `continue-on-error` |
| `build-linux-mingw-wine` | ubuntu-24.04 | MinGW-w64 + Wine | Release | `workflow_dispatch` only, `continue-on-error`, experimental |
| `build-macos` | macos-15 | Apple Clang | Debug, Release | `continue-on-error` |
| `coverage` | ubuntu-24.04 | GCC | Debug | `--coverage` + lcov |
| `clang-tidy` | ubuntu-24.04 | Clang | Debug | blocking job; per-check diagnostic budget ratchet (`Tools/clang-tidy-budget.json`) |
| `todo-count` | ubuntu-24.04 | -- | -- | fails above 20 (required) |
| `docs-health` | ubuntu-24.04 | -- | -- | required; docs exact-currentness, docs contract and link validation (DOC-410) |

**Enforcement truth (verified 2026-09-12):** legacy branch protection is not
configured on `Working` (`branches/Working/protection` is 404), but repository
ruleset `21968740` (`Working integrity`) is active. It protects against deletion
and non-fast-forward updates and requires the GitHub Actions `Required CI Gate`
check with no bypass actors. The exact-source gate accepts a failed Build job only if
`build.yml` at that exact commit declares the job `continue-on-error`; the
required set is cross-checked between `required-ci-gate.needs` and
`EXPECTED_REQUIRED_JOBS_JSON`, and a required job marked `continue-on-error` is
rejected outright. A non-required job may also be `skipped` when its exact
committed job-level `if:` is an allowlisted event-only guard that is false for
the verified source event. Today that covers `build-linux-mingw-wine`
(`workflow_dispatch` only) and `Coverage PR Comment` (`pull_request` only) on a
push. Any other `if:` expression, a required job, or a guard that is true for
the event still rejects the skip. Re-verified 2026-09-24 with
`GITHUB_TOKEN=$(gh auth token) python3 .github/scripts/verify-working-ruleset.py --live`
(GitHub omits `bypass_actors` without an admin-scoped token, and the verifier then
fails closed with `bypass_actors not visible`). That command asserts
that ruleset `21968740` is active, has no bypass actors, and requires exactly
`Required CI Gate` from integration 15368. CI runs its fixture tests in
`validate-ci-tools`. Every `tools/validate-all.sh` check except the advisory
`check-bloat.sh` and warn-only `check-wiki-quality.sh` now runs fail-closed in a
required job. `test-workflow-failure-propagation.py` enforces that mapping.
Documentation health is the required `docs-health` job in `build.yml`. It runs
`docs/update-all-docs.sh check`, `tools/docs_contract.py validate`,
`tools/site-data/validate_docs_links.py` and the hostile docs tests, so a stale
generator, a missing generator result or a broken link fails `Required CI Gate`.
It moved from `site-data.yml`, which is not a required check and whose push runs
cancel each other. `test-workflow-failure-propagation.py` rejects dropping it from
the gate's needs or expected inventory, a second copy in `site-data.yml`, and any
`continue-on-error`, `set +e` or `|| true` around its checks.
A Build Matrix Verifier run conclusion is never evidence (each
source attempt fires the workflow twice; the `in_progress` run skips verification
and still concludes success; never add `run-name` to that workflow, because GitHub
then returns it as the run's API name, which the exact gate compares to the
workflow name). Sanitizer classification gained `incomplete-run`
(suite died before writing JUnit, or no terminal Results marker), which outranks
`sanitizer-finding`; job logs carry a bounded excerpt of the runtime report.

The CI-100 fail-closed control is available only through an explicit manual
dispatch input. Run `gh workflow run build.yml --ref Working -f simulate_required_job_failure=true` against the commit under test. A valid
control result has `validate-ci-tools` fail at the
`Controlled required-job failure probe` step and `Required CI Gate` fail after
observing that required dependency; this red run is proof of failure propagation,
not release evidence. The input defaults to false, and push/PR runs cannot enable
the probe.

`Required CI Gate` also publishes a machine-readable record of its decision.
`verify-required-jobs.py --json-out required-ci-gate.json` writes a canonical
`sparkengine.required-ci-gate.v1` JSON document with sorted keys. It holds the
exact `sha`, `run_id`, `run_attempt`, `repository`, `event` and `ref`, the
expected job list, each job's raw `result` and `status`, the deferred and failed
entries, and the `verdict` (`pass`/`fail`). The record is written on pass and on
fail, and the exit code is unchanged (0 pass, 1 failed job, 2 invalid evidence).
Invalid needs evidence or a malformed run identity writes no record and removes
any stale one. The gate uploads it under `if: always()` as
`required-ci-gate-<sha>-<run_attempt>` with `if-no-files-found: error`, so a red
gate still publishes the record. It is additive: no consumer reads it yet, and
none should until a hosted run has published one.

### Code Coverage

The `coverage` CI job produces lcov reports showing line and branch coverage. To generate coverage locally:

```bash
# Build with coverage flags
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="--coverage" \
  -DCMAKE_C_FLAGS="--coverage"
cmake --build build --parallel $(nproc)

# Run tests to generate coverage data
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..

# Generate coverage report (requires lcov)
lcov --capture --directory build --output-file coverage.info
lcov --remove coverage.info '/usr/*' 'ThirdParty/*' 'Tests/*' --output-file coverage.filtered.info
genhtml coverage.filtered.info --output-directory coverage-report
```

Open `coverage-report/index.html` in a browser to view the report.

### Running Sanitizer Builds Locally

#### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..
```

Or use the preset:

```bash
cmake --preset ci-linux-asan
cmake --build build/ci-linux-asan
ctest --test-dir build/ci-linux-asan --output-on-failure --no-tests=error
```

#### ThreadSanitizer

```bash
cmake --preset ci-linux-tsan
cmake --build build/ci-linux-tsan
ctest --test-dir build/ci-linux-tsan --output-on-failure --no-tests=error
```

### Matching CI Locally

To reproduce a specific CI failure, match the exact compiler and flags:

```bash
# Linux GCC (matches build-linux-gcc)
cmake -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTS=ON
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..

# Linux Clang (matches build-linux-clang)
cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTS=ON
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure --no-tests=error && ./bin/SparkTests && cd ..
```

### Clang-Format Check

The CI enforces formatting on every PR. To check locally:

```bash
find SparkEngine/Source GameModules/SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format --dry-run --Werror 2>&1
```

To auto-fix:

```bash
find SparkEngine/Source GameModules/SparkGame/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src \
  -not -path '*/Metal/*' \
  \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' \) | \
  xargs clang-format -i
```

---

## Live Editor Testing (Software Rendering)

The SparkEditor can be tested with full graphics on headless Linux using Xvfb and Mesa llvmpipe software rendering. An automated test script exercises the editor UI, menus, panels, and keyboard shortcuts.

### Quick Start

```bash
# Start virtual framebuffer
Xvfb :99 -screen 0 1920x1080x24 -ac &

# Set environment
export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3

# Run the automated live-editor test suite
python3 tools/test-editor-live.py build/bin/SparkEditor
```

### Editor Test Mode Flags

| Flag | Description |
|------|-------------|
| `--test-mode` | Skip project browser, enable debug console |
| `--test-frames N` | Exit after N rendered frames |
| `--debug-console` | Print diagnostics to stdout |

### What Gets Tested

- Editor launch and clean shutdown
- OpenGL 3.3 rendering via Mesa llvmpipe
- ImGui frame rendering (non-blank, sufficient UI complexity)
- Menu bar interaction (File, Edit, Window menus)
- Panel toggle via Window menu (Scene View, Asset Browser, Profiler, Material Editor)
- Keyboard shortcuts (Ctrl+Z, Ctrl+S, Ctrl+N)
- Hierarchy panel click and right-click context menu
- 5-second stress test (no crashes)

### Prerequisites

System packages: `xvfb`, `libgl-dev`, `xdotool`, Python 3 with Pillow.
SDL2 must be built with OpenGL/GLX support (install `libgl-dev` *before* building SDL2).

---

## See Also

- [Build System and CMake Modules](Build-System-and-CMake-Modules.md) -- BUILD_TESTS flag and CI details
- [Getting Started](../getting-started/Getting-Started.md) -- Building the project
- [Contributing](Contributing.md) -- Contribution workflow, pre-commit checks, and adding tests

## Test File Inventory

<!-- AUTO:test_inventory -->
*715 test-bearing `.cpp`/`.mm` files, 8339 source-level test definitions*

| Test File | Test Definitions |
|-----------|------------------|
| `TestSubsystemIntegrationScenarios` | 5 |
| `TestAIBehaviorTree` | 16 |
| `TestAIBudgetLimiter` | 6 |
| `TestAIDebugRenderer` | 7 |
| `TestAIDebugRendererPhaseDD` | 9 |
| `TestAIDebugRendererReal` | 8 |
| `TestAIDirector` | 11 |
| `TestAIDirectorPhaseII` | 8 |
| `TestAIIntegratedSystem` | 11 |
| `TestAIStress` | 18 |
| `TestASSET220GamePackagerReal` | 4 |
| `TestAbilitySystem` | 27 |
| `TestAbilitySystemReal` | 9 |
| `TestAccessibility` | 15 |
| `TestAchievementSystem` | 14 |
| `TestAchievementSystemReal` | 10 |
| `TestAdvancedAssetPipeline` | 5 |
| `TestAdversarialEngine` | 96 |
| `TestAlignedHeapArray` | 6 |
| `TestAlignedHeapArrayReal` | 6 |
| `TestAngelScriptEngine` | 14 |
| `TestAngelScriptStackAlignmentReal` | 11 |
| `TestAngleUtils` | 10 |
| `TestAngleUtilsReal` | 7 |
| `TestAnimNotify` | 10 |
| `TestAnimationCompression` | 6 |
| `TestAnimationCompressionReal` | 5 |
| `TestAnimationPhysicsIntegration` | 8 |
| `TestAnimationRetargeting` | 9 |
| `TestAnimationStress` | 12 |
| `TestAnimationSystem` | 17 |
| `TestAreaAssetLoader` | 18 |
| `TestAreaSimulationHook` | 6 |
| `TestAssertSuppression` | 9 |
| `TestAssertSuppressionReal` | 8 |
| `TestAssetDependencyGraph` | 19 |
| `TestAssetManifestReal` | 4 |
| `TestAssetMigration` | 23 |
| `TestAssetMigrationPhaseEE` | 10 |
| `TestAssetParserHardening` | 8 |
| `TestAssetPipelineCache` | 22 |
| `TestAssetPipelineIntegration` | 16 |
| `TestAssetPipelineReal` | 19 |
| `TestAssetServiceClient` | 13 |
| `TestAssetStallDetector` | 9 |
| `TestAssetValidator` | 7 |
| `TestAsyncComputeScheduler` | 9 |
| `TestAsyncComputeSchedulerPhaseCC` | 12 |
| `TestAsyncDatabase` | 23 |
| `TestAsyncDatabaseRegressions` | 3 |
| `TestAtomicSharedPtr` | 3 |
| `TestAtomicSharedPtrReal` | 7 |
| `TestAudioBackendFactory` | 5 |
| `TestAudioECSBindingReal` | 3 |
| `TestAudioEngine` | 18 |
| `TestAudioEngineReal` | 11 |
| `TestAudioMixerBus` | 7 |
| `TestAutoLODPerformance` | 2 |
| `TestBVHAccelerator` | 10 |
| `TestBehaviorTreeNodes` | 22 |
| `TestBenchmarkFramework` | 17 |
| `TestBitFlags` | 14 |
| `TestBitFlagsReal` | 4 |
| `TestBitUtils` | 10 |
| `TestBitUtilsReal` | 8 |
| `TestBlendSpace` | 6 |
| `TestBlendSpaceReal` | 6 |
| `TestBuildInfraReal` | 4 |
| `TestCSGEditorPanel` | 10 |
| `TestCSGSystem` | 12 |
| `TestCacheDebuggerPhaseFF` | 9 |
| `TestCachedShadowAtlas` | 13 |
| `TestCameraInterpolation` | 9 |
| `TestCameraTransforms` | 26 |
| `TestChromeTracing` | 5 |
| `TestClientPrediction` | 12 |
| `TestClothSimulation` | 7 |
| `TestClusteredLightGPU` | 7 |
| `TestCollaborativeEditing` | 33 |
| `TestCollisionAvoidance` | 8 |
| `TestCollisionLayers` | 10 |
| `TestCollisionSystem` | 22 |
| `TestColorUtils` | 18 |
| `TestColorUtilsReal` | 4 |
| `TestCommandHistory` | 10 |
| `TestCompressionUtils` | 4 |
| `TestCompressionUtilsReal` | 7 |
| `TestConditionSystem` | 12 |
| `TestConfigParser` | 17 |
| `TestConfigParserReal` | 10 |
| `TestConnectionScope` | 8 |
| `TestConnectionScopeFilter` | 5 |
| `TestConnectionScopeWiring` | 6 |
| `TestConnectionTimeout` | 9 |
| `TestConsoleProcessPipeReal` | 6 |
| `TestConsoleRBAC` | 21 |
| `TestConsoleVariables` | 31 |
| `TestConstantBufferDiff` | 8 |
| `TestConstantBufferDiffReal` | 9 |
| `TestConstantBufferRing` | 9 |
| `TestContainerUtils` | 10 |
| `TestContainerUtilsReal` | 10 |
| `TestContracts` | 6 |
| `TestCooldown` | 14 |
| `TestCooldownReal` | 9 |
| `TestCoreAndBuildSystems` | 39 |
| `TestCoroutineScheduler` | 13 |
| `TestCoverSystem` | 4 |
| `TestCoverSystemReal` | 5 |
| `TestCoverageAI` | 9 |
| `TestCoverageCamera` | 4 |
| `TestCoverageScripting` | 6 |
| `TestCpuDebuggerPhaseGG` | 8 |
| `TestCpuNeuralInference` | 14 |
| `TestCpuNeuralTraining` | 13 |
| `TestCrashArtifactRetention` | 5 |
| `TestCrashHandlerGatingReal` | 8 |
| `TestCrashSymbolication` | 5 |
| `TestCrossSystemIntegration` | 4 |
| `TestD3D11DeviceContractsReal` | 17 |
| `TestDATA120BackupRestore` | 16 |
| `TestDATA120PersistenceReal` | 18 |
| `TestDATA120SecretsAtRest` | 5 |
| `TestDXRSupport` | 13 |
| `TestDaemonCodexFixes` | 4 |
| `TestDaemonConcurrent` | 6 |
| `TestDaemonDiagnostics` | 11 |
| `TestDaemonFoundation` | 5 |
| `TestDaemonLRU` | 15 |
| `TestDaemonLifecycle` | 11 |
| `TestDaemonProtocol` | 10 |
| `TestDataTableSystem` | 11 |
| `TestDatablockRegistry` | 10 |
| `TestDatablockRegistryPhaseHH` | 8 |
| `TestDayNightCycle` | 10 |
| `TestDeadlockDetector` | 8 |
| `TestDebugHookManager` | 29 |
| `TestDebugTools` | 37 |
| `TestDebugUtilities` | 28 |
| `TestDecalSystem` | 7 |
| `TestDedicatedServer` | 27 |
| `TestDedicatedServerProcessController` | 5 |
| `TestDedicatedServerRuntime` | 13 |
| `TestDeferredDeletion` | 6 |
| `TestDeferredDeletionReal` | 6 |
| `TestDeferredQueue` | 6 |
| `TestDelegate` | 9 |
| `TestDelegateReal` | 8 |
| `TestDeltaSmoother` | 10 |
| `TestDeltaSmootherReal` | 8 |
| `TestDenoiserInterface` | 14 |
| `TestDescriptorCache` | 4 |
| `TestDestructionSystem` | 5 |
| `TestDialogueStress` | 10 |
| `TestDialogueSystem` | 8 |
| `TestDirectStorageLoader` | 12 |
| `TestDirectionalStreaming` | 3 |
| `TestDirtyRectTracker` | 9 |
| `TestDirtyRectTrackerReal` | 12 |
| `TestDirtyRegionGridPhaseDD` | 10 |
| `TestDocumentDurableWrite` | 4 |
| `TestDocumentInterruptionReal` | 3 |
| `TestDrawIndirect` | 6 |
| `TestDynamicQualityScalerPhaseBB` | 11 |
| `TestDynamicResponseSystem` | 6 |
| `TestECSIntegration` | 9 |
| `TestECSStress` | 10 |
| `TestECSWorld` | 11 |
| `TestECSystemOrdering` | 15 |
| `TestECSystemSpecialized` | 27 |
| `TestECSystemsReal` | 12 |
| `TestEDT210InspectorEditCommitReal` | 8 |
| `TestENG200ScriptAudioAnimationReal` | 5 |
| `TestENG200ScriptBindingsReal` | 10 |
| `TestENG200ScriptFaultsReal` | 7 |
| `TestENG200ScriptHotReloadReal` | 10 |
| `TestENG200VisualScriptRuntimeReal` | 3 |
| `TestENG220GPUSkinningD3D11Real` | 4 |
| `TestENG220ObjImportReal` | 5 |
| `TestEcsCameraConsole` | 1 |
| `TestEditorAssetDrag` | 5 |
| `TestEditorAssetDropWorldReal` | 5 |
| `TestEditorAssetReference` | 3 |
| `TestEditorAutomation` | 9 |
| `TestEditorCommands` | 8 |
| `TestEditorCookPackageReal` | 2 |
| `TestEditorCrashHandlerFilterReal` | 11 |
| `TestEditorDocumentReal` | 5 |
| `TestEditorDocumentTransition` | 7 |
| `TestEditorGizmoTransformReal` | 6 |
| `TestEditorLayoutManager` | 16 |
| `TestEditorPanelsRealBackends` | 12 |
| `TestEditorProjectMaterializationReal` | 5 |
| `TestEditorRecovery` | 18 |
| `TestEditorStateCompatibility` | 8 |
| `TestEditorSubsystems` | 138 |
| `TestEditorSubsystemsReal` | 16 |
| `TestEditorUndoHierarchyReal` | 9 |
| `TestEditorUndoWorldMatrixReal` | 2 |
| `TestEditorUntrustedProject` | 5 |
| `TestEditorWindowManager` | 14 |
| `TestEngineBootPlatforms` | 41 |
| `TestEngineDiagnostics` | 4 |
| `TestEngineLifecycle` | 22 |
| `TestEngineLoadTest` | 22 |
| `TestEngineMonitor` | 10 |
| `TestEngineSettingsEdgeCases` | 45 |
| `TestEngineSettingsParser` | 28 |
| `TestEngineSettingsReal` | 14 |
| `TestEngineWiringReal` | 12 |
| `TestEntityArchetype` | 5 |
| `TestEntityEventBus` | 11 |
| `TestEntityEventBusReal` | 6 |
| `TestEntityPresetManager` | 10 |
| `TestEntityPresetManagerPhaseEE` | 7 |
| `TestEnvironmentQuery` | 12 |
| `TestEventBus` | 15 |
| `TestEventBusReal` | 8 |
| `TestEventResponseSystem` | 15 |
| `TestEventResponseSystemPhaseEE` | 8 |
| `TestEventSystem` | 10 |
| `TestExecScript` | 11 |
| `TestExtendedSystems` | 38 |
| `TestFBXImportValidation` | 3 |
| `TestFBXImporter` | 17 |
| `TestFPSComponentsReal` | 11 |
| `TestFPSGameplayIntegration` | 17 |
| `TestFPSLANLoopback` | 2 |
| `TestFPSMultiplayer` | 17 |
| `TestFPSWeatherPort` | 6 |
| `TestFastNoise2SIMD` | 32 |
| `TestFaultIsolation` | 14 |
| `TestFaultIsolationReal` | 8 |
| `TestFileUtils` | 20 |
| `TestFileUtilsReal` | 7 |
| `TestFileWatcher` | 11 |
| `TestFilesystemLinks` | 2 |
| `TestFixtures` | 4 |
| `TestFogSystem` | 17 |
| `TestFoliageImpostorBaker` | 21 |
| `TestFoliageRenderer` | 31 |
| `TestFoliageSystem` | 10 |
| `TestFontSystem` | 13 |
| `TestFormationSystem` | 9 |
| `TestFrameAllocator` | 8 |
| `TestFreezeDetector` | 10 |
| `TestFreezeSystem` | 5 |
| `TestFrustumCulling` | 11 |
| `TestFullEngineDiagnostics` | 9 |
| `TestGLSLPipelineIntegration` | 19 |
| `TestGLTFAnimationImport` | 27 |
| `TestGLTFSkinnedMeshLoader` | 19 |
| `TestGLTFStaticMeshLoader` | 10 |
| `TestGPUClusterCulling` | 11 |
| `TestGPUDrivenRenderer` | 14 |
| `TestGPUDrivenRendererD3D11` | 2 |
| `TestGPUParticleSystem` | 11 |
| `TestGPUPerfCounters` | 9 |
| `TestGPUProfiler` | 10 |
| `TestGPUResourceLeakDetector` | 10 |
| `TestGPUStallProfiler` | 6 |
| `TestGPUStallProfilerPhaseCC` | 10 |
| `TestGTAOEffect` | 8 |
| `TestGameModeReal` | 11 |
| `TestGameModuleMMO` | 46 |
| `TestGameModulePlatformerARPG` | 37 |
| `TestGameModuleRPG` | 35 |
| `TestGameModuleRTS` | 39 |
| `TestGameModuleRacing` | 29 |
| `TestGameObjectTransforms` | 24 |
| `TestGamePackager` | 14 |
| `TestGameViewPanel` | 3 |
| `TestGamepadInputProcessing` | 23 |
| `TestGameplayDebugger` | 11 |
| `TestGameplayExtensionRegistry` | 7 |
| `TestGameplayStress` | 15 |
| `TestGameplaySystemExtension` | 6 |
| `TestGameplayTags` | 14 |
| `TestGameplayTagsReal` | 7 |
| `TestGatewayAreaControl` | 24 |
| `TestGatewaySecurity` | 14 |
| `TestGizmoMath` | 3 |
| `TestGoldenImageTest` | 28 |
| `TestGraphicsBenchmarkStats` | 3 |
| `TestGraphicsEngine` | 14 |
| `TestGraphicsEngineLinuxPassTruthReal` | 2 |
| `TestGraphicsInitFallback` | 7 |
| `TestGraphicsIntegration` | 33 |
| `TestGraphicsStress` | 15 |
| `TestGraphicsSubsystems` | 45 |
| `TestGroupAI` | 5 |
| `TestHEAD220NullRHILifetimeReal` | 8 |
| `TestHLODBuilderPhaseII` | 8 |
| `TestHLODSystem` | 9 |
| `TestHRTFProcessor` | 8 |
| `TestHResultPlatform` | 24 |
| `TestHash` | 18 |
| `TestHashReal` | 9 |
| `TestHeadlessTickStats` | 7 |
| `TestHitchDetector` | 10 |
| `TestHybridRT` | 20 |
| `TestInGameConsole` | 12 |
| `TestInputActionSystem` | 12 |
| `TestInputBindings` | 5 |
| `TestInputFrameEdgesReal` | 5 |
| `TestInputSystem` | 11 |
| `TestInstanceManager` | 14 |
| `TestInventorySystem` | 11 |
| `TestInventorySystemReal` | 11 |
| `TestJobSystem` | 14 |
| `TestJsonStrict` | 20 |
| `TestJsonUtils` | 24 |
| `TestLIFE200LifecycleLoopReal` | 7 |
| `TestLIFE200ModuleReloadLoopReal` | 3 |
| `TestLIFE200ModuleReloadReal` | 5 |
| `TestLODGenerator` | 7 |
| `TestLODGeneratorPhaseGG` | 9 |
| `TestLagCompensation` | 12 |
| `TestLagCompensationIntegration` | 4 |
| `TestLauncherPaths` | 4 |
| `TestLauncherProcess` | 7 |
| `TestLegacyGameObjectMaterial` | 5 |
| `TestLevelStreamingSystemPhaseAA` | 11 |
| `TestLifecycleCompositionRootFailure` | 10 |
| `TestLightManager` | 13 |
| `TestLightmapBaker` | 9 |
| `TestLoadingScreen` | 11 |
| `TestLoadingScreenReal` | 5 |
| `TestLocalFileCache` | 15 |
| `TestLocalizationSystem` | 6 |
| `TestLockFreeRingAllocator` | 8 |
| `TestLockFreeRingAllocatorReal` | 10 |
| `TestLogger` | 20 |
| `TestLoggerSinksReal` | 4 |
| `TestLootAndCrafting` | 11 |
| `TestMMOAssetImport` | 16 |
| `TestMMOCredentialSecurity` | 3 |
| `TestMOD300ShowcaseLocalizationReal` | 2 |
| `TestMOD310FPSSceneReloadRespawnReal` | 6 |
| `TestMOD320MMOPersistenceReal` | 17 |
| `TestMOD330ARPGDungeonReal` | 7 |
| `TestMOD330ARPGWorldActors` | 4 |
| `TestMOD340PlatformerCompletionReal` | 12 |
| `TestMOD340PlatformerProgressReal` | 2 |
| `TestMOD350RPGNPCNavigationReal` | 6 |
| `TestMOD350RPGQuestSliceReal` | 9 |
| `TestMOD360OpenWorldPersistenceReal` | 5 |
| `TestMOD360OpenWorldTraversalReal` | 5 |
| `TestMOD370RTSSaveReal` | 3 |
| `TestMOD370SkirmishDeterminismReal` | 11 |
| `TestMOD380RacingCompleteRaceReal` | 12 |
| `TestMOD380VehiclePhysicsReal` | 5 |
| `TestMOD390VisualScriptDiagnosticsReal` | 7 |
| `TestMOD390VisualScriptGameplayReal` | 4 |
| `TestMOD390VisualScriptGraphsReal` | 14 |
| `TestMOD390VisualScriptHotReloadReal` | 4 |
| `TestMSanCanary` | 2 |
| `TestMacOSPlatform` | 6 |
| `TestMain` | 1 |
| `TestMaterialDefinition` | 10 |
| `TestMaterialEffects` | 5 |
| `TestMaterialSystemEdgeCases` | 10 |
| `TestMaterialSystemIntegration` | 14 |
| `TestMaterialSystemReal` | 13 |
| `TestMaterialSystemValidation` | 31 |
| `TestMathUtils` | 11 |
| `TestMathUtilsExtendedPhaseGG` | 5 |
| `TestMemoryDebugger` | 16 |
| `TestMemoryIntegrity` | 20 |
| `TestMemoryIntegrityLifecycle` | 4 |
| `TestMemoryMonitor` | 11 |
| `TestMeshLOD` | 8 |
| `TestMeshOptimizer` | 15 |
| `TestMeshShaderPipeline` | 9 |
| `TestMetalRayTracing` | 16 |
| `TestMetalRayTracingLive` | 10 |
| `TestModSystem` | 9 |
| `TestModuleABI` | 39 |
| `TestModuleABIDiagnostics` | 6 |
| `TestModuleDependency` | 5 |
| `TestModuleDiscovery` | 7 |
| `TestModuleHotReload` | 12 |
| `TestModuleLifecycleReal` | 16 |
| `TestModuleVersion` | 6 |
| `TestMovementSystem` | 18 |
| `TestMovieRenderPipeline` | 11 |
| `TestMultiISADispatch` | 14 |
| `TestMusicManager` | 9 |
| `TestNET100Handshake` | 12 |
| `TestNET100Libsodium` | 1 |
| `TestNET100TransportReal` | 16 |
| `TestNET100TrustStore` | 6 |
| `TestNavMesh` | 11 |
| `TestNavMeshLink` | 5 |
| `TestNavMeshObstacles` | 7 |
| `TestNetBuffer` | 29 |
| `TestNetImpairmentWiring` | 7 |
| `TestNetQuantize` | 12 |
| `TestNetTransportSecurity` | 11 |
| `TestNetworkDebugPanel` | 11 |
| `TestNetworkEncryption` | 17 |
| `TestNetworkHealthMonitor` | 10 |
| `TestNetworkIntegration` | 32 |
| `TestNetworkInterpolation` | 12 |
| `TestNetworkMMOIntegration` | 11 |
| `TestNetworkManagerEdgeCases` | 37 |
| `TestNetworkManagerIntegration` | 39 |
| `TestNetworkManagerOrchestration` | 27 |
| `TestNetworkManagerReal` | 24 |
| `TestNetworkReplicationIntegration` | 13 |
| `TestNetworkSecurity` | 6 |
| `TestNetworkSecurityPhaseHH` | 2 |
| `TestNetworkStack` | 4 |
| `TestNetworkStress` | 21 |
| `TestNeuralInference` | 17 |
| `TestNeuralPostProcessing` | 9 |
| `TestNeuralRadianceCache` | 7 |
| `TestNeuralTextureCompressor` | 10 |
| `TestNeuralWeightsValidation` | 6 |
| `TestNoiseGenerator` | 7 |
| `TestNullRHIDevice` | 7 |
| `TestNullRHIDevicePhaseY` | 22 |
| `TestObjectPool` | 6 |
| `TestObjectPoolReal` | 7 |
| `TestOcclusionCulling` | 6 |
| `TestOnlineServices` | 26 |
| `TestOnlineServicesLocalStack` | 6 |
| `TestOpaqueHandle` | 7 |
| `TestOpenWorldModule` | 61 |
| `TestPLT210AngelScriptVector3Real` | 2 |
| `TestPLT210ProcessPosixReal` | 7 |
| `TestPLT210RuntimeProcessesReal` | 3 |
| `TestPacketValidator` | 10 |
| `TestPacketValidatorReal` | 3 |
| `TestParallelCulling` | 5 |
| `TestPasswordHash` | 3 |
| `TestPathCache` | 6 |
| `TestPerceptionSystemMath` | 25 |
| `TestPerformanceStats` | 10 |
| `TestPerformanceStatsReal` | 4 |
| `TestPersistentMaterialCB` | 19 |
| `TestPhysicsComponents` | 22 |
| `TestPhysicsECSIntegration` | 10 |
| `TestPhysicsInterpolation` | 8 |
| `TestPhysicsStress` | 16 |
| `TestPhysicsSystem` | 29 |
| `TestPhysicsTeardownGuard` | 7 |
| `TestPlatformInput` | 11 |
| `TestPlayModeManager` | 34 |
| `TestPluginABI` | 17 |
| `TestPortalCulling` | 14 |
| `TestPoseModifier` | 8 |
| `TestPostProcessingPipeline` | 16 |
| `TestPostProcessingPipelineD3D11` | 1 |
| `TestPostProcessingPipelinePhaseJ` | 20 |
| `TestPostProcessingPipelinePhaseK` | 11 |
| `TestPostProcessingPipelinePhaseN` | 7 |
| `TestPrefabCompatibility` | 5 |
| `TestPrefabPersistence` | 12 |
| `TestProceduralGenerator` | 14 |
| `TestProcess` | 20 |
| `TestProcessDrawListLinux` | 10 |
| `TestProfiler` | 19 |
| `TestPrototypeModuleKitReal` | 3 |
| `TestProximityTriggerSystem` | 4 |
| `TestQuaternionStubsReal` | 14 |
| `TestQuestSystem` | 11 |
| `TestRHI210D3D11DeviceLossReal` | 3 |
| `TestRHI210D3D11GoldenReal` | 4 |
| `TestRHI210D3D11ValidationReal` | 4 |
| `TestRHI225D3D12FallbackReal` | 3 |
| `TestRHI225D3D12ParityReal` | 12 |
| `TestRHI225D3D12ValidationReal` | 5 |
| `TestRHI230VulkanGoldenReal` | 4 |
| `TestRHI230VulkanValidationReal` | 18 |
| `TestRHI240OpenGLGoldenReal` | 8 |
| `TestRHI240OpenGLReal` | 13 |
| `TestRHIBridgeIntegration` | 19 |
| `TestRHICapabilityParity` | 4 |
| `TestRHIHandlePool` | 10 |
| `TestRHIHandlePoolPhaseX` | 15 |
| `TestRTHandleSystem` | 15 |
| `TestRandomEngine` | 11 |
| `TestRecastIntegration` | 6 |
| `TestReflectedScene` | 17 |
| `TestReflectedSceneCompatibility` | 3 |
| `TestReflectedSceneEmissiveHierarchy` | 6 |
| `TestReflection` | 18 |
| `TestReflectionProbeCache` | 16 |
| `TestReflectionReal` | 27 |
| `TestRegionMapDataSource` | 7 |
| `TestReliableChannel` | 23 |
| `TestRemoteDebugSystem` | 22 |
| `TestRenderCommandRing` | 8 |
| `TestRenderECSIntegration` | 8 |
| `TestRenderGraph` | 36 |
| `TestReplaySystem` | 8 |
| `TestReplicationFields` | 15 |
| `TestResult` | 8 |
| `TestRingBuffer` | 14 |
| `TestRingBufferReal` | 7 |
| `TestRunnerSemanticsReal` | 17 |
| `TestRuntimePackage` | 2 |
| `TestRuntimePackageContentRoots` | 4 |
| `TestRuntimePrefab` | 20 |
| `TestSAVE230NewerFormatSlotReal` | 2 |
| `TestSDK240ModuleDiagnostics` | 3 |
| `TestSEC100ChatAuditLogReal` | 3 |
| `TestSEC100RemoteAdminUnavailableReal` | 4 |
| `TestSEC2ConsoleIpcReal` | 11 |
| `TestSEC2GameModules` | 15 |
| `TestSEC2PersistenceHardening` | 13 |
| `TestSEC2RenderingHardeningReal` | 8 |
| `TestSEC3GameplayHardening` | 13 |
| `TestSEC3ScriptingHardening` | 11 |
| `TestSEC4NarrowPathsReal` | 13 |
| `TestSEC4SoundEffectWav` | 8 |
| `TestSHLighting` | 7 |
| `TestSSAOTemporalFilter` | 8 |
| `TestSafetyCoreUtils` | 17 |
| `TestSaveInterruptionReal` | 3 |
| `TestSaveSystemRoundTripReal` | 18 |
| `TestSceneConfigDatabase` | 3 |
| `TestSceneConfigDatabaseReal` | 9 |
| `TestSceneGraph2D` | 14 |
| `TestSceneManager` | 21 |
| `TestSceneManagerUnicodeReal` | 1 |
| `TestSceneRoundtrip` | 8 |
| `TestSceneSaveConfinedReal` | 5 |
| `TestSceneSerializer` | 13 |
| `TestSceneSerializerReal` | 22 |
| `TestSceneSnapshotSerializer` | 20 |
| `TestScheduledCallback` | 8 |
| `TestScopeGuard` | 13 |
| `TestScopedTimer` | 7 |
| `TestScreenCapture` | 9 |
| `TestScreenSpaceEffects` | 16 |
| `TestScriptHookManager` | 15 |
| `TestScriptHookManagerPhaseBB` | 14 |
| `TestScriptHotReload` | 16 |
| `TestScriptSandbox` | 7 |
| `TestSeamlessAreaManager` | 14 |
| `TestSecDaemonCacheHardening` | 8 |
| `TestSecDaemonPipeIdentity` | 5 |
| `TestSecureRandom` | 3 |
| `TestSecureTransportWired` | 20 |
| `TestSecurityParsersReal` | 26 |
| `TestSelectionManager` | 23 |
| `TestSelfRecovery` | 16 |
| `TestSequencer` | 10 |
| `TestSequencerAudioWiring` | 3 |
| `TestSequencerReal` | 9 |
| `TestSerializationHardening` | 9 |
| `TestSerializer` | 17 |
| `TestServerLiveMockClient` | 10 |
| `TestServerMockClient` | 31 |
| `TestServiceTopologyController` | 10 |
| `TestSessionCompatibilityReal` | 11 |
| `TestShaderCompilerReal` | 15 |
| `TestShaderCrossCompilerPhaseW` | 21 |
| `TestShaderDiskCache` | 6 |
| `TestShaderDiskCacheDaemon` | 9 |
| `TestShaderDiskCachePhaseV` | 17 |
| `TestShaderGraphCompiler` | 8 |
| `TestShaderHotReload` | 11 |
| `TestShaderHotReloadCompilation` | 11 |
| `TestShaderHotReloadPhaseU` | 9 |
| `TestShaderServiceClient` | 11 |
| `TestShaderVariantSystem` | 21 |
| `TestShadowAtlas` | 7 |
| `TestShadowPassReal` | 11 |
| `TestSkyAtmosphere` | 5 |
| `TestSoftwareRendering` | 5 |
| `TestSparkBuildConfig` | 9 |
| `TestSparkConsoleConcurrency` | 3 |
| `TestSparkEngineCameraOwnership` | 1 |
| `TestSparkError` | 6 |
| `TestSparkGameFPSLoopReal` | 35 |
| `TestSparkGameFPSMirrorCompanionsReal` | 19 |
| `TestSparkGameShowcase` | 6 |
| `TestSparkGatewayCoordinator` | 15 |
| `TestSparkPak` | 24 |
| `TestSparkServerApplication` | 28 |
| `TestSparkServerHealth` | 8 |
| `TestSparkTerrainFormatReal` | 2 |
| `TestSpatialGrid` | 16 |
| `TestSpatialGridReal` | 7 |
| `TestSplineMath` | 24 |
| `TestSplineMathReal` | 7 |
| `TestSpringArm` | 6 |
| `TestSpringArmReal` | 8 |
| `TestSprite2DComponents` | 35 |
| `TestStackTrace` | 17 |
| `TestStartupSplash` | 7 |
| `TestStateMachine` | 16 |
| `TestStateMachineReal` | 7 |
| `TestSteeringBehaviors` | 15 |
| `TestSteeringBehaviorsReal` | 5 |
| `TestStringPool` | 8 |
| `TestStringUtils` | 20 |
| `TestStringUtilsReal` | 9 |
| `TestSubTickInput` | 5 |
| `TestSubsystemConsoleCommands` | 14 |
| `TestSystemManagerIntegration` | 11 |
| `TestTF120Residency` | 19 |
| `TestTF120SharedSaveRoot` | 8 |
| `TestTFAbilityWire` | 6 |
| `TestTFCaptureMath` | 7 |
| `TestTFChatRules` | 11 |
| `TestTFDamageModel` | 18 |
| `TestTFDataTables` | 23 |
| `TestTFDeathRecapWire` | 5 |
| `TestTFFixedStep` | 1 |
| `TestTFLanBeaconCodec` | 3 |
| `TestTFLoadoutWire` | 3 |
| `TestTFNetProtocolLayout` | 9 |
| `TestTFObservation` | 4 |
| `TestTFOnboarding` | 45 |
| `TestTFOutfitStore` | 18 |
| `TestTFRedeployRules` | 7 |
| `TestTFRegionLattice` | 11 |
| `TestTFScramAuth` | 11 |
| `TestTFSecondaryMotion` | 7 |
| `TestTFServerSecurity` | 9 |
| `TestTFServerValidation` | 19 |
| `TestTFSocialStore` | 9 |
| `TestTacticalPointSystem` | 4 |
| `TestTelemetry` | 16 |
| `TestTelemetryPhaseFF` | 7 |
| `TestTelemetrySpool` | 9 |
| `TestTemplateRuntimeReal` | 7 |
| `TestTemplatesCompile` | 43 |
| `TestTemporalEffects` | 11 |
| `TestTerrainRenderer` | 5 |
| `TestTextureCompressor` | 12 |
| `TestTextureCompressorPhaseGG` | 6 |
| `TestTextureStexBoundsReal` | 11 |
| `TestTextureZombiePool` | 6 |
| `TestThirdPartyIntegration` | 27 |
| `TestThreadDebugger` | 22 |
| `TestThreadSafeQueue` | 10 |
| `TestTimeOfDaySystem` | 18 |
| `TestTimerManager` | 11 |
| `TestTimerManagerReal` | 8 |
| `TestTimerReal` | 6 |
| `TestTransientBufferAllocator` | 10 |
| `TestTransientBufferAllocatorPhaseX` | 14 |
| `TestTutorialSystem` | 22 |
| `TestTween` | 14 |
| `TestTweenReal` | 8 |
| `TestTypeTraits` | 11 |
| `TestUICompositor` | 13 |
| `TestUILayoutExtensions` | 29 |
| `TestUISystem` | 6 |
| `TestUISystemPhaseR` | 7 |
| `TestUUID` | 12 |
| `TestUndoRedoManager` | 7 |
| `TestUndoRedoManagerProduction` | 3 |
| `TestUnwiredSystemRemoval` | 3 |
| `TestUpscalingSystem` | 10 |
| `TestUserDataPathsReal` | 7 |
| `TestUtilsStress` | 13 |
| `TestVRAMTelemetry` | 5 |
| `TestVRSystem` | 12 |
| `TestVersionControlSystemGitReal` | 5 |
| `TestVersionControlSystemPhaseAA` | 11 |
| `TestVersionedHandle` | 9 |
| `TestVideoPlayer` | 12 |
| `TestVisualScriptCompiler` | 28 |
| `TestVolumeManager` | 11 |
| `TestVolumetricClouds` | 14 |
| `TestVoxelConeTracing` | 24 |
| `TestVulkanLavapipe` | 5 |
| `TestWARPRendering` | 4 |
| `TestWaterRenderer` | 6 |
| `TestWeaponMechanicsReal` | 22 |
| `TestWeaponSystem` | 18 |
| `TestWeatherSystem` | 8 |
| `TestWindowsCommandLine` | 8 |
| `TestWorkSema` | 7 |
| `TestWorldBasicRender` | 8 |
| `TestWorldOriginSystem` | 12 |
| `TestWorldServerConcurrency` | 3 |
| `TestWorldServerRouting` | 22 |
| `Test_ai-anim_animation` | 3 |
| `Test_ai-anim_navmesh` | 2 |
| `Test_core_hardening` | 5 |
| `Test_ecs_ai_pathfollow` | 4 |
| `Test_ecs_audio_doppler` | 4 |
| `Test_editor_collab_lock_protocol` | 3 |
| `Test_engine-misc_Coroutine` | 2 |
| `Test_engine-misc_EventQueueEviction` | 3 |
| `Test_engine-misc_MobileGestures` | 3 |
| `Test_engine-misc_SnapshotCount` | 2 |
| `Test_gamemodules_mmochat_di` | 1 |
| `Test_gameplay_achievement` | 2 |
| `Test_gameplay_dialogue` | 1 |
| `Test_gameplay_instance` | 2 |
| `Test_gameplay_inventory` | 2 |
| `Test_gameplay_response` | 3 |
| `Test_graphics_rhi` | 6 |
| `Test_lifecycle_ecs_phase_wiring` | 4 |
| `Test_net-world_migration` | 2 |
| `Test_persistence_AsyncDatabaseParams` | 2 |
| `Test_persistence_AsyncDatabasePool` | 4 |
| `Test_persistence_ModSystem` | 2 |
| `Test_persistence_ReplaySystem` | 3 |
| `Test_persistence_SaveSystem` | 43 |
| `Test_scripting_hardening` | 8 |
| `Test_tests_ecsystemordering_real` | 6 |
| `Test_tests_enginecontext_real` | 2 |
| `Test_tests_inversekinematics` | 10 |
| `Test_tooling_CommandParser` | 9 |
| `Test_ui-2d_tween` | 7 |
| `Test_ui-2d_ui` | 3 |
<!-- /AUTO:test_inventory -->
