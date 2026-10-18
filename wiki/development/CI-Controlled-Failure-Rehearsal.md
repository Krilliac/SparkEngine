# CI Controlled-Failure Rehearsal (CI-100)

> **Audience:** Programmers, QA
>
> **Thread Context:** N/A (development/CI process reference)
>
> **Platform/Backend Scope:** GitHub Actions `Build SparkEngine` workflow (ubuntu-24.04 / windows runners)

## Overview

CI-100[0] requires evidence that a **controlled test, sanitizer, format,
threshold, registration, and validation failure each make the `Required CI Gate`
turn red** — proof that the gate is fail-closed, not merely green by habit. This
page describes the rehearsal that produces that evidence on demand, from one
maintainer command, **without changing production CI behaviour**.

The rehearsal applies a tiny, defect-introducing patch on a throwaway branch,
watches the real `Build SparkEngine` run go red, records the run, and tears the
branch down. The patches are never merged; applying one introduces a defect the
gate is designed to catch, so nothing here weakens a gate.

## When to Use

- Producing the hosted CI-100[0] evidence (an owner task; see below).
- Confirming, after a workflow change, that each required check still reddens the
  gate.
- Reviewing which intended job and step each class reaches.

## The six classes and their gated jobs

| Class | Patch (`tools/ci/controlled-failures/`) | Required job that turns red | Real check |
|-------|------------------------------------------|-----------------------------|-----------|
| test | `test.patch` | `build-linux-gcc` | `ctest` → `SparkTests` returns `EXIT_FAILURE` |
| sanitizer | `sanitizer.patch` | `build-linux-asan` | AddressSanitizer heap-use-after-free, `halt_on_error=1` |
| format | `format.patch` | `check-format` | clang-format 18 `--dry-run --Werror` |
| threshold | `threshold.patch` | `coverage` | `scripts/coverage-report.sh` per-subsystem threshold |
| registration | `registration.patch` | `validate-ci-tools` | `tools/check-test-registration.sh` |
| validation | `validation.patch` | `docs-health` | `tools/site-data/validate.py` (readiness path check) |

Every one of these jobs is listed in `required-ci-gate.needs` and in the gate's
`EXPECTED_REQUIRED_JOBS_JSON` (`.github/workflows/build.yml`), so a non-success
result in any of them makes `verify-required-jobs.py` report the gate as
`failure`. The driver verifies the intended step's failure; the map is pinned by
`.github/scripts/test-workflow-failure-propagation.py`
(`test_ci100_controlled_failure_classes_map_to_gated_required_jobs`), which runs
in the required `validate-ci-tools` job.

## Local proofs (patch applied vs. reverted)

Each patch was verified to fail its real check with the patch applied and pass
without it:

- **format** — pinned clang-format 18.1.3 (`--dry-run --Werror`) flags the
  patched file (exit 1); clean after revert.
- **registration** — `tools/check-test-registration.sh` reports
  `MISSING REGISTRATION: Tests/TestCI100RegistrationProbe.cpp` (exit 1); clean
  after revert.
- **validation** — `python3 tools/site-data/validate.py` fails with
  `referenced path does not exist` (exit 1), as does the `docs-health`
  `Tests/Tools/test_site_data_contract.py` suite; clean after revert.
- **threshold** — `scripts/coverage-report.sh` on a synthetic lcov fixture with
  the `Core` threshold raised to 100 % reports `Core … FAIL >= 100%` (exit 1);
  the baseline 40 % threshold passes. A real instrumented coverage build is the
  hosted `coverage` job. A 100 % threshold can be met in principle; the
  fixture only proves the check rejects its below-threshold measurement.
- **test** / **sanitizer** — the probe bodies were compiled against the real
  `Tests/TestFramework.h` with a minimal runner. `EXPECT_TRUE(false)` produces a
  hard failure and a non-zero process exit; the AddressSanitizer build of the
  heap-use-after-free probe reports `heap-use-after-free` and aborts. The
  end-to-end full-suite red (the whole `SparkTests` corpus under `ctest` / the
  ASan lane) is produced by the hosted rehearsal, not re-built locally.

## Running the rehearsal (owner, with authenticated `gh`)

```bash
# Dry run (default) — prints the plan, touches nothing, needs no gh auth:
python3 tools/ci/run_controlled_failure_rehearsal.py --base <base-sha>

# Execute — pushes rehearsal branches, opens draft do-not-merge PRs, records
# evidence, then closes the PRs and deletes the branches:
python3 tools/ci/run_controlled_failure_rehearsal.py --base <base-sha> --execute
```

For each class the driver creates `rehearsal/ci100-<class>-<shortsha>-<run-id>` from the
base SHA, applies the patch, opens a **draft** PR against `Working` labelled
`do-not-merge`, waits for the `Build SparkEngine` run, records the run id, the
failing job(s), intended failing step, patch SHA, PR URL, and the `Required CI Gate` conclusion into
`docs/readiness/evidence/ci100-controlled-failures.json`, then closes the PR and
deletes the branch after checking its SHA. A class is marked `demonstrated` only
when a `pull_request` Build run belongs to that PR and exact pushed patch SHA,
the intended job and step fail, and the gate and run conclude `failure`.
Cleanup failures are recorded and make the command fail. Unit tests for the driver (fake `gh`, no network) live
in `tools/ci/test_run_controlled_failure_rehearsal.py`.

## Two controlled-failure paths and owner decision

`build.yml` already has a default-off `workflow_dispatch` input,
`simulate_required_job_failure`. When explicitly enabled, its probe fails the
`Controlled required-job failure probe` step in `validate-ci-tools`. A dispatch
from `Working` can establish generic propagation from one required job to the
gate at that ref. It does not exercise the six defect classes above.

The draft-PR rehearsal exercises six class-specific checks with patches based
on a selected SHA. Its runs are `pull_request` events targeting `Working`, not
pushes or dispatches on `Working`. Even six demonstrated PR records do not by
themselves settle the work item's Working-branch definition of done.

CI-100[0]'s definition of done asks for a controlled required-job failure that
turns the gate red **"on Working."** The rehearsal runs on draft `do-not-merge`
pull requests whose base is `Working`, because the `Build SparkEngine` workflow
treats `pull_request` events into `Working` with the same required job set and
the same always-running gate as a `Working` push (no `paths`/`paths-ignore`
filters; the gate is `if: always()` and needs the full inventory). Whether a
draft-PR red run satisfies the "on Working" wording, or whether a direct push to
`Working` (temporarily, behind the ruleset) is required, is an **owner
decision**. Until that decision and the required hosted evidence exist,
CI-100[0] remains `unmet`. A dispatch probe can complement the PR records, but
neither local fake-runner tests nor a generic dispatch substitutes for six
class-specific hosted failures.

## Troubleshooting

- **A class is not marked `demonstrated`.** Inspect the recorded `runId` and
  `patchSha`: the PR identity, exact SHA, intended job, failing step, Build
  conclusion, and gate conclusion must all match.
- **`git apply` fails for a patch.** The base SHA drifted from where the patch
  was captured. Re-capture the patch against the current base (edit the file,
  `git diff` it, revert), keeping the change minimal and single-class.
- **The rehearsal reports cleanup errors.** Inspect the named PR and branch.
  Close only the PR created by this run; delete a branch only after confirming
  it still points to the recorded `patchSha`. A hard interrupt can skip cleanup.

## Related Pages

- [CI Reproducible Builds](CI-Reproducible-Builds.md)
- [Testing](../advanced/Testing.md)
