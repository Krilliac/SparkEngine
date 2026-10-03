# GitHub API — PR Checks Diagnosis

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (development/process reference)
>
> **Platform/Backend Scope:** All platforms (`gh` CLI + git)

## Overview

The full workflow for polling PR checks, diagnosing failures, and rerunning jobs via the `gh` CLI. Paired with [Build Optimizations](Build-Optimizations.md) (which explains *why* the steps below are ordered as they are) and [Workflow Patterns](Workflow-Patterns.md) (which shows where this fits in the post-push flow).

## 1 — Poll

Use `gh pr checks --watch` **only** in an interactive terminal. In any scripted or non-TTY context, use the snapshot form:

```bash
gh pr checks                                # one-shot snapshot
gh pr checks --json name,state,conclusion   # machine-readable
```

Exit code 1 from `gh pr checks` while checks are still running is **not** a failure signal — ignore it and re-poll. See the `--watch` caveat in [Build Optimizations](Build-Optimizations.md).

### Exhausted commit-status contexts

GitHub limits a SHA/context pair to 1,000 commit statuses. Its explicit HTTP 422
status-cap rejection is permanent for that pair; another pending write cannot
restore capacity. The trusted aggregate terminal publisher preserves the original
error without a recovery POST for that specific response. Other uncertain errors
retain the existing fail-closed pending recovery, authenticated evidence checks
and pending ownership rules.

This guard does not repair an initial pending-write failure or implement semantic
deduplication. Do not retry the exhausted pair or rename its required context to
bypass the gate. See [GitHub's commit-status limit](https://docs.github.com/rest/commits/statuses#create-a-commit-status).

## 2 — Identify the failing job

```bash
gh run list --branch "$(git branch --show-current)" --limit 5
```

Columns: `STATUS · CONCLUSION · WORKFLOW · BRANCH · RUN_ID · STARTED`. Grab the RUN_ID of the failed run.

## 3 — Download only the failed job logs

```bash
gh run view <RUN_ID> --log-failed
```

`--log-failed` returns only the jobs whose conclusion is `failure`. Full logs (`--log`) can run 10-50 MB across the full matrix and are almost always unnecessary.

## 4 — Narrow to the first error line

The relevant failure is almost always the first `error:` or `FAILED` line in each job:

```bash
gh run view <RUN_ID> --log-failed 2>&1 | grep -nE "error:|FAILED|undefined" | head -20
```

If the root cause is buried (e.g. CMake reports a link error but the real issue was a missing symbol upstream), also scan for the first `ninja: build stopped` and read the preceding diagnostic context.

Note: this repo also has a `report-ci-errors` job that aggregates per-job error summaries and uploads them as artifacts (each build job uploads a `ci-errors-*` / `sanitizer-report-*` artifact). When present, downloading that artifact can be faster than scraping raw logs.

## 5 — Reproduce locally

Pull the matching configuration from `.github/workflows/build.yml`. The standard Linux jobs use manual flags, but for the common Debug/Release cases the presets match closely enough:

```bash
cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release --parallel $(nproc)
cmake --preset linux-gcc-debug   && cmake --build build/linux-gcc-debug --parallel $(nproc)
```

See [CI Reproducible Builds](CI-Reproducible-Builds.md) for the full job ↔ command table (including the sanitizer jobs, which use manual `cmake -B build` flag invocations, not presets).

## 6 — Fix, push, re-poll

```bash
git add -u && git commit --amend --no-edit
git push --force-with-lease
sleep 15
gh pr checks --fail-fast
```

`--force-with-lease` refuses the push if the remote moved since your last fetch — safer than plain `--force`.

## Notes

- Always fetch before polling if the PR is shared — someone else may have already pushed a fix.
- If the failing job is marked `continue-on-error: true` in `build.yml`, the PR can still merge. As of this writing those are `build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, and `clang-tidy`. Don't block on those unless the user explicitly asks.
- `gh pr view --json statusCheckRollup` returns the aggregate check state in one call — useful when you only care about red/green.

## Source & Freshness

- Original entry: `GitHub API — PR Checks Diagnosis`, last updated 2026-04-17.
- Verified against codebase 2026-06-08.
- Updated / found stale:
  - Added the `report-ci-errors` job and the `ci-errors-*` / `sanitizer-report-*` artifacts as a faster alternative to scraping raw logs (new since the source was written).
  - Verified the `continue-on-error` job list against `.github/workflows/build.yml`: `build-windows-vs2026`, `build-linux-mingw-wine`, `build-macos`, `clang-tidy` (matches `CLAUDE.md`).
  - Clarified that the sanitizer jobs are reproduced via manual flags, not presets, pointing to the CI Reproducible Builds page.
  - Retargeted cross-references to the migrated wiki pages.

## Related Pages

- [Build Optimizations](Build-Optimizations.md) — `--log-failed`, `--watch` caveats, `--parallel $(nproc)` for rebuilds
- [CI Reproducible Builds](CI-Reproducible-Builds.md) — job ↔ command table
- [Workflow Patterns](Workflow-Patterns.md) — post-push verification flow
- [Project conventions (CLAUDE.md)](../../CLAUDE.md) — "Post-PR checks" section

## Combined source diagnostic qualification

The isolated `codex/combined-qualification-20261002` push workflow runs one
Clang Debug row, one Clang Release row, the full existing TSan job, coverage,
and one VS2026 Release row against its exact source commit. Existing test
selections, count floors, sanitizer evidence and coverage thresholds remain.
Each job is bounded to 120 minutes on an existing standard hosted runner;
permissions remain contents-read. Product artifact packaging is omitted.
It does not dispatch Wine or SDE rows or replace Required CI Gate.

The Windows row additionally checks the owner's three approved Primary WARP
images at their exact hashes and zero tolerance, then requires each disabled
pass to cause a real pixel mismatch while unaffected scenes still match.
These diagnostics preserve disabled CTest flags and cannot promote release
acceptance. The generated SDK consumer test also loads its exact module through
the installed host; compilation or sidecar validation alone is insufficient.

## Reliability follow-up source qualification

The isolated combined-reliability branch runs one20-minute Ubuntu source-contract
job. It verifies the exact checkout, cleanup/report contracts, the full existing
license/supply-chain checker, workflow propagation and privileges, documentation
currentness/determinism and inventory. It uses the runner's existing Python/YAML
environment and adds no dependency install, compiler, engine execution, artifact
publication or native matrix. These checks do not transfer the earlier native
candidate results to a new source SHA. Its draft targets the combined candidate
branch, so the existing Working/claude-targeted native and analysis workflows are
not duplicated by this source-only qualification.

## Primary verifier follow-up

The primary-verifier follow-up uses the existing 20-minute source-only workflow
and explicitly runs both `test_rhi210_capture.py` and
`test_verify_approved_primary.py`. Capture requests a separate runner output log
so the existing test framework retains per-case JUnit assertion details.
Geometry mutation must change geometry and downstream lighting; lighting and
shadow mutations affect only their respective scenes. Strict assertion evidence,
approved image hashes, zero tolerance and unchanged scenes remain required.
Source tests do not qualify native rendering. All four normal/control invocations
still need exact-binary native evidence; no earlier result is retroactively accepted.
This workflow installs no dependencies and starts no native build.

## Bounded Primary native qualification

The isolated Primary native workflow builds Windows Release once on the existing
standard VS2026 runner/cache setup, pinned to product source
`858978d87d72388578f89aa5b56fa65bd25a12a9`. It records separate workflow/source
identities and the executable SHA256, then runs normal capture and the three
disabled-pass controls. Per-capture 120-second limits and the 120-minute job limit
are unchanged. A failed capture stops subsequent controls; there is no automatic
job retry. JUnit assertion details, pixel verdicts and native identity are retained.
This diagnostic route does not run or replace the full CTest, SDK or stable-v1
certification gates. No baseline/tolerance, renderer, compiler version, dependency
version or release readiness claim changes.

### Diagnostic compiler-cache key contract

Combined/Primary diagnostics save the exact cache action primary-key output and
restore their own combined key family before the existing fallbacks. Configuration,
toolset and build-input hash scopes are retained; existing caches are not deleted.
The source-only workflow exercises reachability, duplicate-prefix rejection and
cross-configuration/toolset rejection. This corrects the observed doubled-prefix
save key but does not solve the previous run's 2,390 multiple-input non-cacheable
compiler calls. Actual invocation/response-file evidence is needed before any
compiler flag change. The active pinned native run is not modified or retried.

## Final installed-host ABI rejection driver

`Tests/PackageSmoke/run_installed_module_abi_rejection.py` complements the
same-host positive lifecycle test with the existing benign SDK N+1/N-1 fixtures.
Supply an exact final MinSizeRel installed prefix, both fixture DLLs/sidecars,
an external short ASCII evidence root, and the declared source SHA. The native
Windows driver checks image hashes and exact SDK-only metadata drift, selects
each module explicitly, and requires exit 2, the exact pre-OS-load mismatch
diagnostic, zero lifecycle callbacks and no execution sentinel. Crashes, timeouts,
wrong paths/versions or input mutation fail and retain diagnostic evidence.

Each case starts outside the installed prefix with fresh LOCALAPPDATA/APPDATA;
the engine may reanchor a manifest-bearing package's cwd, while existing user-path
handling routes normal logs, settings, saves and trace to the isolated user root.
The source-only workflow runs metadata/verdict and mocked orchestration tests;
it executes no native host. CLI source/configuration labels are declarations,
not binary attestation: final build/install manifests must bind measured hashes,
and a positive lifecycle result must use the same installed host hash.

Example for a separately coordinated native qualification (not this source CI):

```text
python -B Tests/PackageSmoke/run_installed_module_abi_rejection.py --installed-root <final-prefix> --newer <MinSizeRel>/SparkMismatchedModuleFixture.dll --previous <MinSizeRel>/SparkPreviousSdkModuleFixture.dll --evidence-root <external-short-ASCII-directory> --source-sha <exact-final-SHA> --configuration MinSizeRel
```

The active Primary job is Release rendering capture only. It provides no installed
MinSizeRel ABI qualification. This driver does not build/install, alter ABI rules,
replace signature/installer checks, or qualify real predecessor upgrade/rollback.

## Installed MinSizeRel native diagnostic

The separate bounded Windows 2022/v143 route builds exact product source `f92d28016fc74e5076591173b4ac5518415fed2f` and runs installed SDK lifecycle, the benign incompatible-module controls, and package closure on the same host image. It preserves Shipping policies and existing job/test ceilings. Workflow/source identities and measured image hashes are separate. See [execution and evidence scope](../../.github/scripts/installed-native-qualification.md). This staged hosted diagnostic does not replace Windows 11, MSI, signing or release gates.

## Requested installed SDK consumer configuration

The installed SDK template runner explicitly sets `CMAKE_CONFIGURATION_TYPES` to its requested `SPARK_CONFIG`. Ninja Multi-Config defaults omit MinSizeRel, so configuring without this argument let the host build succeed but rejected the Shipping consumer before compilation. The compiler-free regression exercises the production configure arguments: the old argument set lacks `build-MinSizeRel.ninja`, while the corrected set builds a LANGUAGES NONE marker target. Compiler selection, SDK boundaries, sidecars and lifecycle criteria remain unchanged.

## Atomic save interruption suite ownership

The existing exact-three SparkSaveInterruptionTests lane owns the AtomicWrite_ family, retaining warning/error/serial/timeout requirements. The main suite excludes this already-duplicated family only when that lane is registered, using the existing coverage-audited helper. Native qualification must execute both main and dedicated lanes; this handoff does not establish timing improvement or resolve unfinished CPU-floor checking.

## Coherent Windows readiness qualification

The isolated readiness candidate contains the AtomicWrite family handoff and all preceding combined source fixes, plus the previously separate PR589 shipped-root clang-tidy changes. The PR588 workflow lint hardening is already inherited. Existing diagnostic-only branches and original branches remain intact. PR589 imports preserve every prior file/check allowance and only its reviewed one-warning `bugprone-exception-escape` allowance; this is not a new full-tree tidy measurement.

The candidate-only workflow gates two standard Windows Server 2022/v143 jobs on source contracts. Both build the same immutable product revision, separately from the workflow revision: one all-target Release build followed by unfiltered full CTest and required CPU-floor results, and one MinSizeRel build followed by installed SDK lifecycle, both ABI fixtures and closure graphs. Prebuild registrations are checked against postbuild discovery before policy/profile validation. Actual disabled CTest records are accounted separately; no timeout, test-count, coverage or approved-image threshold is relaxed. Four Primary controls run against the Release binary with its hash preserved. No result transfers from an older source revision.

Each native job has a 240-minute ceiling with owned child cleanup. Release SDE uses the already pinned URL/hash, bounded transfer and four logged stages; no unchanged retry is scheduled. Full framework/JUnit/CPU/Primary proof is retained or fails on overflow. Raw Release process logger streams remain explicitly identified diagnostic tails. Installed SDK/ABI stdout and stderr are now retained completely up to 128 KiB each, failing on overflow or invalid UTF-8. Text evidence remains bounded to the existing per-route caps, with no binary artifact download. Windows 11 hardware, Wine/DXVK, protected signing and final release evidence remain separate obligations.

The combined Windows workflow pins product source `d90b86e42fd586a56d53f077bbca6a457200d9bd` for both native jobs. Its workflow revision differs only in qualification binding/documentation and generated metadata. The original standalone diagnostic branches retain their historical source identities.

## Pinned SDE extraction preflight

Combined run37098288198 downloaded the pinned Intel archive and verified its digest, then reached Windows System32 tar extraction before the unchanged 280-second owned-phase deadline expired. Its untimed stage markers do not establish how the budget was split or prove an underlying tar defect. A separate setup-only preflight tests Python standard-library extraction of that same archive with UTC stage timestamps, extraction metadata and the same phase/dependency limits. It performs no engine build, CPU-floor test or installed qualification. Existing full-suite/CPU-floor requirements remain mandatory; setup success alone cannot satisfy them. The MinSizeRel job from that combined run continues independently and must not be duplicated for extraction diagnosis.

## Arena acceptance and Release continuation

The installed arena audit parser requires one Loop response for each accepted game_status before the next command or end of the audit. A previously completed response cannot stand in for a missing final response; duplicate and unsolicited Loop records also fail. Existing completion thresholds, timeout and configuration eligibility remain unchanged. The compiler-free self-test reproduces stale acceptance on the previous parser and covers all 18 rejection cases after the correction. This supporting Release lane does not enable developer gameplay commands in Shipping or certify a MinSizeRel playthrough.

The successful same-archive SDE setup probe permits the Release-only continuation to qualify exact product `d90b86e42fd586a56d53f077bbca6a457200d9bd` with one all-target build, full CTest/CPU-floor accounting and unchanged Primary controls. The corrected extractor runs from the workflow checkout. Existing workflow definitions and required gates remain intact. The completed installed MinSizeRel SDK/ABI/closure job and complete offline replay already qualify that same product; no installed rebuild is scheduled.

The newer arena parser fix is preserved in reviewed source commit `c5a879f648544c14e40dd620069ce5a775d83fb4`. It is source-tested only and is not part of the d90b86e42 native qualification. Final Windows 11/Shipping certification and other release requirements remain separate.

## ISA failure diagnostic scope

Release qualification run37101493564 built product d90b86e42 and accounted for all 437 CTest results. Its sole failure was CpuFloor_IsaBaseline: 74 undecodable entries near Spark::Json::Detail::EscapeString in SparkGameMMOFPS.dll, with zero recognized above-floor instructions. The missing original byte/PDB/disassembly evidence prevents a justified scanner correction. The original gate stays failed.

A separate target-only diagnostic preserves the same immutable product, Release configuration, prerequisites and deadlines, building only SparkGameMMOFPS and required dependencies. It runs the unchanged strict scanner and collects bounded findings, bytes, PDB/function boundaries and disassembly. The rebuilt DLL hash is compared with the original runner-attested hash; matching it does not prove original PDB identity. This diagnostic cannot qualify the full suite or replace the completed installed result. Evidence remains UTF-8 text only, at most 1 MiB per proof JSON, 1 MiB archived and 12 MiB expanded; incomplete capture fails closed. No additional scanner allowance or classification change is made.
