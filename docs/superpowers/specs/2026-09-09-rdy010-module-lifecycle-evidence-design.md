# RDY-010 Stable-v1 Module Lifecycle Evidence Design

## Purpose

Close the `lifecycle-log` producer gap for the sole stable-v1 module,
`SparkGameFPS`, with evidence produced by a real Windows engine process that
loads the exact built DLL. This design does not claim to close RDY-010: the
separate `package-smoke-log` gap remains owned by MOD-310.

## Current failure

The repository already has two incompatible partial contracts:

- `ModuleManager` counts aggregate successful lifecycle callbacks and the
  Windows host prints `SPARK_MODULE_LIFECYCLE` after normal teardown for the
  existing `ModuleProfileLifecycle_SparkGameFPS_D3D11` CTest.
- `tools/module-evidence/collect_lifecycle.py` instead expects freely emitted
  `[module-lifecycle] <module> <phase>` lines and invokes obsolete
  `-headless -test-frames` arguments. It cannot launch the production Windows
  path or produce its declared JSON artifact.

No checked-in CI job runs the collector against a real stable-v1 DLL, and the
module-evidence validator therefore permits the declared lifecycle gap.

## Scope and non-goals

In scope:

- Capture module-scoped, post-teardown counts for `CreateModule`, `OnLoad`,
  `OnUpdate`, `OnFixedUpdate`, `OnRender`, `OnUnload`, and `DestroyModule`.
- Emit exactly one host-owned, standalone structured record for the required
  initialized game module after teardown on the `-require-game` Windows
  execution path.
- Make the collector launch that same real D3D11/WARP process using an exact
  DLL path from a downloaded Windows build artifact.
- Add a named `module-profile-lifecycle` Windows CI job and make
  `module-evidence` consume its artifact fail-closed.
- Remove only the `lifecycle-log` row from `evidence-gaps.json` after the
  producer and consumer exist.

Out of scope:

- Package-smoke evidence or clean-layout certification (MOD-310).
- Treating same-workflow artifacts as independently attested release
  authority; CI-120 remains responsible for protected external attestation.
- Experimental modules outside stable-v1.
- Changing normal interactive runtime logging or the module ABI.

## Chosen architecture

### Module-scoped counters

`ModuleManager::LifecycleEvidence` becomes a teardown snapshot containing a
bounded list of records keyed by the loaded module's `ModuleInfo::name`. Each
record holds non-negative counts for the observable factory and callback
phases plus guarded-dispatch faults. The aggregate counters remain for
existing consumers, but every successful lifecycle transition also updates
the record for its owning `LoadedModule`.

The manager records `CreateModule` after a non-null factory return and after
the module name is known, records `OnLoad` only when it returns true, records
update/fixed-update/render only after their guarded callback completes,
records `OnUnload` after it returns, and records `DestroyModule` after the
destroy function completes. A failed load or a fault leaves at least one
required count at zero, which is intentionally non-evidence. Staged hot-reload
manager evidence is merged into the owning manager exactly as current
aggregate evidence is merged; staged managers never publish a separate
teardown snapshot.

### Host-owned terminal record

After ordinary Windows teardown, `SparkEngineWindows.cpp` selects the one
initialized game-module record required by `-require-game` and writes one
direct stdout record:

```
SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=4 fixed=2 render=4 unload=1 destroy=1 faults=0
```

The record is emitted by the host after `ModuleManager` destruction, not via
the logger and not by the module. The parser accepts only a standalone complete
record, rejects malformed/duplicate matching records, and requires exactly
one record for `SparkGameFPS` with positive create/load/update/unload/destroy
counts, at least one fixed/update/render callback, and zero faults. A module
attempting to print a spoofed line creates a duplicate and fails the run.

### Real collector invocation

`collect_lifecycle.py` is refactored around a strict record parser and receives
the actual module image and extracted runtime directory explicitly:

```
python tools/module-evidence/collect_lifecycle.py \
  --engine <runtime>/SparkEngine.exe \
  --module SparkGameFPS \
  --module-image <runtime>/SparkGameFPS.dll \
  --working-directory <runtime> \
  --rhi-backend d3d11 \
  --out build/module-evidence/module-lifecycle.json \
  --commit-sha <exact-sha>
```

It launches the same bounded windowed command as the real CTest:
`-game <DLL> -require-game -test-seconds 1.0 -threads 2 -window-size 640x360
-no-subprocess`, with only `SPARK_RHI_BACKEND=d3d11` and
`SPARK_D3D11_DRIVER=warp` supplied in the child environment. It verifies that
the engine and module image are regular non-reparse binaries under the
extracted runtime root, records SHA-256 for both, writes the captured log next
to the JSON, and writes no JSON on any failure.

The lifecycle JSON schema gains `moduleSHA256` and `modulePath`. The validator
requires their exact types/digests alongside the current source-tree and
engine bindings. This proves what binary was launched; protected provenance of
the workflow artifact remains CI-120's separate requirement.

### CI data flow

```
build-windows-vs2022 (Release) ──uploads exact runtime ZIP──▶ module-profile-lifecycle
module-profile-lifecycle ──uploads lifecycle JSON + log──▶ module-evidence
module-evidence ──validates target/JUnit/lifecycle evidence──▶ required-ci-gate
```

`module-profile-lifecycle` runs on `windows-2022`, depends on the existing
Windows build, downloads only its same-workflow `SparkEngine-Windows-VS2022-
Release` artifact, expands it into a fresh directory, runs the collector, and
uploads `build/module-evidence/module-lifecycle.json` plus its log with the
exact SHA in the artifact name. `module-evidence` depends on both the Linux
JUnit producer and this job, downloads the lifecycle artifact, passes it to
`validate_manifest.py --lifecycle-evidence`, and fails if it is absent,
malformed, cross-SHA, or phase-incomplete. The required aggregate directly
lists the new job, so a skipped or failed producer cannot disappear behind a
green module-evidence result.

## Error handling and security invariants

- No fallback from a missing module image to a name, repository path, or
  arbitrary executable.
- No success JSON after an engine exit, timeout, malformed terminal record,
  duplicate record, zero required count, nonzero fault count, or invalid
  binary/path identity.
- The terminal parser remains line-anchored and accepts no logger-prefixed
  records.
- Evidence records remain bounded: one stable-v1 module record and fixed
  numeric fields; unknown keys are rejected by the JSON validator.
- Every workflow download is named for the current `github.sha`; no artifact
  from another run or revision is accepted as current evidence.

## Verification strategy

1. Unit-test the ModuleManager record accounting with the real compatible
   module fixture, including failure, successful callbacks, unload/destroy,
   and staged reload aggregation.
2. Extend the CMake parser self-test with valid, duplicate, malformed,
   missing-phase, unexpected-module, and nonzero-fault terminal records.
3. Add Python collector tests for strict terminal parsing, exact command/env,
   fake script and reparse rejection, no-output-on-failure, module digest, and
   cross-SHA evidence rejection.
4. Extend workflow policy tests to require the new producer job, same-SHA
   artifact flow, validator argument, required-gate membership, and removal of
   the lifecycle gap only.
5. Build the Windows Release target with `BUILD_GAME_MODULES=ON`, run the
   CTest smoke and collector locally, then run all module-evidence and
   workflow-policy suites. Hosted exact-SHA CI is required before the gap can
   be represented as closed in release evidence.
