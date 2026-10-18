# CI-100 controlled-failure patches

Six minimal, self-contained patches, one per failure class CI-100[0] requires a
controlled red run for. Each patch targets one required CI job and check; it may
also affect other jobs that execute the same tests. The driver requires the
intended job and step to fail before recording proof that the gate turned red.
**None of these
patches is ever merged into `Working`.** They are applied only on short-lived
`rehearsal/ci100-<class>-<shortsha>-<run-id>` branches by
`tools/ci/run_controlled_failure_rehearsal.py`, which opens a draft `do-not-merge`
pull request, records the run, and then closes the PR and deletes the branch.

Applying any patch does not weaken a gate — it introduces a defect the gate is
supposed to catch.

## The six classes

| Class | Patch | Required job that turns red | Gate `needs` line (build.yml) |
|-------|-------|-----------------------------|-------------------------------|
| test | `test.patch` | `build-linux-gcc` (ctest → SparkTests) | 4917 |
| sanitizer | `sanitizer.patch` | `build-linux-asan` (AddressSanitizer) | 4910 |
| format | `format.patch` | `check-format` (clang-format 18) | 4902 |
| threshold | `threshold.patch` | `coverage` (per-subsystem threshold) | 4919 |
| registration | `registration.patch` | `validate-ci-tools` (`check-test-registration.sh`) | 4900 |
| validation | `validation.patch` | `docs-health` (site-data/readiness validator) | 4927 |

All six jobs are in `required-ci-gate.needs` and in `EXPECTED_REQUIRED_JOBS_JSON`
(build.yml line 4937), so a failure in any of them makes the gate report
`failure`. `.github/scripts/test-workflow-failure-propagation.py`
(`test_ci100_controlled_failure_classes_map_to_gated_required_jobs`) pins this
map; `verify-required-jobs.py` turns any non-success required job into a red gate.

## What each patch does

- **test.patch** — appends `TEST(CI100_ControlledTestFailure){ EXPECT_TRUE(false); }`
  to the already-registered `Tests/TestMathUtils.cpp`. The runner records a hard
  failure and `main()` returns `EXIT_FAILURE` (`Tests/TestMain.cpp`), so
  `ctest --no-tests=error` turns `build-linux-gcc`'s "Run Tests" step red.
  Other jobs that run `SparkTests`, including coverage, may fail too.
- **sanitizer.patch** — appends an `#if defined(__SANITIZE_ADDRESS__)`-guarded
  heap-use-after-free `TEST` to the registered `Tests/TestObjectPool.cpp`. Under
  the ASan lane (which defines `__SANITIZE_ADDRESS__` and runs with
  `halt_on_error=1`) the freed-memory read aborts the process. Outside a
  sanitizer build the body is inert (`EXPECT_NO_CRASH`), so no other lane is
  perturbed and the per-lane test-count ratchet floors/ceilings are unaffected.
- **format.patch** — adds redundant spaces inside one existing expression in
  `Tests/TestMathUtils.cpp`. No new symbol, no test-count change; it only trips
  `clang-format --dry-run --Werror`.
- **threshold.patch** — raises the `Core` per-subsystem coverage threshold in
  `scripts/coverage-report.sh` from 40 % to 100 %. The synthetic fixture's
  below-threshold result fails; the hosted `coverage` result must be checked.
- **registration.patch** — adds `Tests/TestCI100RegistrationProbe.cpp` **without**
  registering it in `Tests/CMakeLists.txt` and without the
  `// test-registration: ignore` opt-out, so `tools/check-test-registration.sh`
  reports MISSING REGISTRATION and the `validate-ci-tools` job fails.
- **validation.patch** — points one `entryPoints` path of a readiness work item
  (`docs/readiness/work-items/40-installer-governance-docs.json`, item INST-130)
  at a non-existent file, which `tools/site-data/validate.py` rejects
  ("referenced path does not exist"). `docs-health` runs this validator
  (`validate.py --docs` and `Tests/Tools/test_site_data_contract.py`).

## Reproducing the local proofs

Each patch was proven to fail the real check with it applied and pass without it.
From the repository root:

```bash
# format (pinned clang-format 18.1.3, matching CI)
git apply tools/ci/controlled-failures/format.patch
"$CF18" --dry-run --Werror Tests/TestMathUtils.cpp     # exits 1; clean after revert
git checkout -- Tests/TestMathUtils.cpp

# registration
git apply tools/ci/controlled-failures/registration.patch
bash tools/check-test-registration.sh                   # exits 1 (MISSING REGISTRATION)
git apply -R tools/ci/controlled-failures/registration.patch

# validation
git apply tools/ci/controlled-failures/validation.patch
python3 tools/site-data/validate.py                     # exits 1 (referenced path does not exist)
git checkout -- docs/readiness/work-items/40-installer-governance-docs.json

# threshold (synthetic lcov fixture; a real coverage build is the hosted job)
git apply tools/ci/controlled-failures/threshold.patch
bash scripts/coverage-report.sh <coverage.info>         # exits 1 (Core FAIL >= 100%)
git checkout -- scripts/coverage-report.sh

# test and sanitizer: the probe bodies are proven against the real
# Tests/TestFramework.h with a minimal runner (see the CI-100 wiki page). The
# end-to-end full-suite red is produced by the hosted rehearsal.
```

`$CF18` is the CI-parity clang-format 18.1.3
(`C:/Users/Nathan/.cache/cf18_1_3/clang_format/data/bin/clang-format.exe` on the
dev host; `apt-get install clang-format` 18.1.3 on ubuntu-24.04).

## Running the rehearsal

Only the repository owner, with an authenticated `gh`, runs:

```bash
python3 tools/ci/run_controlled_failure_rehearsal.py --base <sha>            # dry run (default)
python3 tools/ci/run_controlled_failure_rehearsal.py --base <sha> --execute  # creates draft PRs
```

Evidence lands in `docs/readiness/evidence/ci100-controlled-failures.json`.
