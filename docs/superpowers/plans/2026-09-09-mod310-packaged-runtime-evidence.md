# MOD-310 Packaged Runtime Evidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce and consume exact-SHA evidence that the installed Windows
Shipping package loads the real SparkGameFPS module under both NullRHI and
D3D11/WARP.

**Architecture:** `build-windows-shipping` creates one hash-bound MinSizeRel
MSI artifact.  A dedicated Windows consumer installs that exact artifact on a
fresh runner, runs both runtime contracts, and uploads one canonical
`package-smoke.log`.  The Ubuntu rooted module-evidence consumer reads that
artifact only after the package-smoke producer succeeds.

**Tech Stack:** Python standard library, WiX/CPack, Windows Installer,
PowerShell, CMake, GitHub Actions, existing no-follow Python evidence reader.

**Spec:** `docs/superpowers/specs/2026-09-09-mod310-packaged-runtime-evidence-design.md`

## Global Constraints

- Use the existing `claude/stable-v1-release` canonical checkout only; no
  new branch, worktree, or PR.
- The shipping runtime is `windows-shipping` / `MinSizeRel`, never the generic
  `Release` runtime ZIP.
- The package log is generated only after a fresh MSI install, both backend
  runs, and successful uninstall/no-residue validation.
- The artifact carries `${{ github.sha }}` and the lower-case SHA-256 of the
  exact MSI; no generated log may claim a different revision.
- Keep MOD-310 open until its public-SDK-only and installed single-player/save
  criteria are separately evidenced.
- Do not claim hosted proof until a real exact-SHA workflow completes.

---

### Task 1: Make package-smoke evidence semantically strict

**Files:**
- Modify: `tools/module-evidence/artifacts.py`
- Modify: `tools/module-evidence/validate_manifest.py`
- Modify: `Tests/Tools/test_module_evidence.py`

**Interfaces:**
- `validate_package_smoke_bytes(data, leaf_name, module_name, *, expected_sha)`
  accepts only a closed `package-smoke-v1` record.
- `validate_artifact_bytes(..., expected_sha=None)` threads the expected SHA
  to package-smoke validation without weakening JUnit validation.

- [ ] **Step 1: Add the failing valid-record and rejection tests.**

  Add a canonical record fixture whose required lines are exactly:

  ```text
  [package-smoke] schema=package-smoke-v1
  [package-smoke] product=SparkEngine
  [package-smoke] module=SparkGameFPS
  [package-smoke] profile=stable-v1
  [package-smoke] commit_sha=<40 lower-case hex>
  [package-smoke] msi_sha256=<64 lower-case hex>
  [package-smoke] backend=nullrhi result=PASS
  [package-smoke] backend=d3d11-warp result=PASS
  [package-smoke] exit_code=0
  [package-smoke] PASS
  ```

  Add independent tests for a missing/duplicate line, wrong module/profile,
  wrong SHA, non-hex digest, missing backend, failed backend, and `exit_code`
  other than zero.  Retain a test proving unrelated evidence types are not
  parsed as package-smoke logs.

- [ ] **Step 2: Run the focused tests and observe the intended failure.**

  Run:

  ```powershell
  python -m unittest Tests.Tools.test_module_evidence.TestArtifactSemanticValidation -v
  ```

  Expected: the new canonical-record test fails because the old parser accepts
  a weaker ad-hoc log and has no expected-SHA parameter.

- [ ] **Step 3: Implement the smallest closed parser.**

  Decode already-held UTF-8 bytes, reject duplicate keys and unknown
  `package-smoke` keys, require all ten lines, validate lower-case digest/SHA
  syntax, require the caller's exact `expected_sha`, and preserve the existing
  byte-size/line-count limits.  Thread `expected_sha` from
  `ManifestValidator` into `validate_artifact_bytes`; no temp file or
  pathname reopen is permitted.

- [ ] **Step 4: Re-run focused and full evidence tests.**

  Run:

  ```powershell
  python -m unittest Tests.Tools.test_module_evidence.TestArtifactSemanticValidation -v
  python -m unittest Tests.Tools.test_module_evidence -q
  ```

  Expected: every package fixture validates only through the new strict
  contract; rooted/no-follow tests remain green.

- [ ] **Step 5: Commit the parser slice.**

  ```powershell
  git add tools/module-evidence/artifacts.py tools/module-evidence/validate_manifest.py Tests/Tools/test_module_evidence.py
  git commit -m "fix(evidence): require exact packaged smoke records"
  ```

### Task 2: Produce hash-bound MSI metadata and real backend evidence

**Files:**
- Create: `.github/scripts/write-shipping-package-manifest.py`
- Create: `.github/scripts/test_write_shipping_package_manifest.py`
- Modify: `.github/scripts/qualify-windows-msi.py`
- Modify: `.github/scripts/test_qualify_windows_msi.py`

**Interfaces:**
- `write-shipping-package-manifest.py --packages <dir> --version <X.Y.Z>
  --commit-sha <40-hex> --out <json>` writes one no-BOM
  `spark-shipping-package-v1` manifest for the exact MinSizeRel MSI.
- `qualify(..., package_manifest, ...)` verifies the manifest/MSI digest, then
  creates `logs/package-smoke.log` only after both backend runs and uninstall
  succeed; the workflow uploads that exact file to the Ubuntu consumer.

- [ ] **Step 1: Write manifest-helper tests first.**

  In a temporary directory, create one correctly named MSI byte fixture and
  assert the JSON contains this exact shape:

  ```json
  {"schemaVersion":"spark-shipping-package-v1","commitSHA":"<sha>","profile":"stable-v1","configuration":"MinSizeRel","version":"<version>","msi":"<expected leaf>","sha256":"<lowercase digest>"}
  ```

  Add failures for absent/extra MSI files, wrong leaf name, bad SHA/version,
  and a path that is a symlink.  The helper must not write an output on error.

- [ ] **Step 2: Observe manifest-helper failures.**

  Run:

  ```powershell
  python .github/scripts/test_write_shipping_package_manifest.py
  ```

  Expected: tests fail because the producer helper does not yet exist.

- [ ] **Step 3: Implement the deterministic manifest writer.**

  Use only `pathlib`, `hashlib`, `json`, and atomic no-BOM file publication.
  Require the exact `SparkEngine-<version>-Windows-AMD64-MinSizeRel-Runtime.msi`
  leaf and lower-case 40-character commit SHA.  Do not infer the revision from
  Git or environment.

- [ ] **Step 4: Add qualification red tests for both installed backends.**

  Extend the fake runner in `test_qualify_windows_msi.py` so it writes:

  - one exact NullRHI module-ready/RHI/headless-lifecycle transcript;
  - one exact D3D11/WARP module-lifecycle transcript; and
  - installer identity/uninstall records.

  Assert that the fresh `logs/package-smoke.log` is absent when either backend,
  the MSI hash, or uninstall fails; assert the canonical ten-line record is
  written only on the all-pass path.

- [ ] **Step 5: Run the red qualification tests.**

  Run:

  ```powershell
  python .github/scripts/test_qualify_windows_msi.py
  ```

  Expected: the new backend/log tests fail because qualification currently
  checks only a module-ready NullRHI line and has no package manifest/log API.

- [ ] **Step 6: Implement qualification extensions.**

  Add explicit package-manifest parsing and MSI re-hash checks before install.
  Reuse the current fresh install root and layout validation.  Parse a single
  direct NullRHI terminal set and one direct D3D11/WARP
  `SPARK_MODULE_LIFECYCLE` line; reject logger copies, duplicates, absent
  phases, zero required counts, and nonzero faults.  Run from the installed
  `bin` directory with the installed DLL only.  Emit the canonical log after
  uninstall/no-residue succeeds, otherwise leave it absent and preserve
  diagnostics.

- [ ] **Step 7: Verify producer tests.**

  Run:

  ```powershell
  python .github/scripts/test_write_shipping_package_manifest.py
  python .github/scripts/test_qualify_windows_msi.py
  ```

  Expected: both helper suites pass and failure paths never leave a passing
  package record.

- [ ] **Step 8: Commit the producer slice.**

  ```powershell
  git add .github/scripts/write-shipping-package-manifest.py .github/scripts/test_write_shipping_package_manifest.py .github/scripts/qualify-windows-msi.py .github/scripts/test_qualify_windows_msi.py
  git commit -m "feat(ci): produce packaged FPS smoke evidence"
  ```

### Task 3: Split the shipping package producer from its clean-install consumer

**Files:**
- Modify: `.github/workflows/build.yml`
- Modify: `.github/scripts/test-workflow-failure-propagation.py`
- Modify: `Tests/Tools/test_module_evidence.py`

**Interfaces:**
- `build-windows-shipping` uploads exactly
  `shipping-package-${{ github.sha }}` containing one MSI, its JSON manifest,
  and `SparkEngineGameModules.cmake`.
- `module-profile-package-smoke` needs `build-windows-shipping`, downloads
  that artifact, invokes the qualification helper, and uploads exactly
  `module-profile-package-smoke-${{ github.sha }}`.

- [ ] **Step 1: Add structural workflow red tests.**

  Assert that the shipping job retains the existing `windows-shipping` build
  and `MinSizeRel` CPack command but no longer consumes its own package.  Add
  a dedicated job test requiring:

  ```yaml
  module-profile-package-smoke:
    needs: [build-windows-shipping]
    runs-on: windows-2022
  ```

  Require exact producer/consumer artifact names, `if-no-files-found: error`,
  no `continue-on-error`, and an explicit nonzero-exit propagation after the
  helper call.

- [ ] **Step 2: Run structural tests red.**

  Run:

  ```powershell
  python .github/scripts/test-workflow-failure-propagation.py WorkflowFailurePropagationTests.test_shipping_ci_qualifies_native_msi_without_rebuilding_or_publishing -v
  python -m unittest Tests.Tools.test_module_evidence.TestEvidenceWorkflowContract -v
  ```

  Expected: tests fail because the package qualifier is currently embedded in
  the shipping build job and no package-smoke job/artifact exists.

- [ ] **Step 3: Implement the split handoff.**

  In `build-windows-shipping`, run the new manifest writer immediately after
  successful WiX CPack output, then upload only the MSI, manifest, and exact
  configured module manifest using the SHA-named artifact.  Move clean
  installation/qualification to `module-profile-package-smoke`; it checks out
  the same revision, downloads the artifact to runner temp, writes only
  `msi-qualification/package-smoke.log`, and uploads that log plus diagnostics
  after helper success. The Ubuntu consumer maps the uploaded leaf to its fixed
  `build/module-evidence/package-smoke.log` namespace.

  Keep the evidence upload immediately after qualification: the Windows job
  owns the workspace between producer exit and the SHA-pinned uploader, and no
  repository-controlled command may run in that interval.

- [ ] **Step 4: Verify workflow contracts.**

  Run:

  ```powershell
  python .github/scripts/test-workflow-failure-propagation.py
  python -m unittest Tests.Tools.test_module_evidence.TestEvidenceWorkflowContract -v
  ```

  Expected: a package-producer skip or consumer failure is structurally
  impossible to hide; existing lifecycle workflow assertions remain green.

- [ ] **Step 5: Commit the workflow handoff.**

  ```powershell
  git add .github/workflows/build.yml .github/scripts/test-workflow-failure-propagation.py Tests/Tools/test_module_evidence.py
  git commit -m "feat(ci): smoke exact shipping package in a clean runner"
  ```

### Task 4: Consume the produced record and close only the evidence gap

**Files:**
- Modify: `tools/module-evidence/schema.py`
- Modify: `tools/module-evidence/evidence-gaps.json`
- Modify: `.github/workflows/build.yml`
- Modify: `.github/scripts/test-workflow-failure-propagation.py`
- Modify: `.github/scripts/test-verify-exact-required-gate.py`
- Modify: `Tests/Tools/test_module_evidence.py`
- Modify: `docs/readiness/work-items/00-truth-ci-release.json`
- Modify: `docs/readiness/work-items/30-game-modules.json`
- Modify: `docs/readiness/ENGINE_READINESS_HANDOFF.md`

**Interfaces:**
- `EVIDENCE_PRODUCERS["package-smoke-log"]` names
  `module-profile-package-smoke`.
- `module-evidence` requires and downloads the exact package-smoke artifact
  before its rooted semantic read.
- `required-ci-gate` lists the package-smoke job in both `needs` and its
  exact expected-job JSON.

- [ ] **Step 1: Add red consumer/gate tests.**

  Require `module-evidence` to depend on and download
  `module-profile-package-smoke-${{ github.sha }}` to the fixed
  `build/module-evidence/package-smoke.log` leaf.  Add a required-gate test
  proving a skipped package-smoke job fails the aggregate.  Add a policy test
  that an empty `gaps` array is accepted only when the actual package artifact
  is present and semantically valid.

- [ ] **Step 2: Run the red tests.**

  Run:

  ```powershell
  python .github/scripts/test-verify-exact-required-gate.py
  python -m unittest Tests.Tools.test_module_evidence.TestEvidenceGapLedger Tests.Tools.test_module_evidence.TestEvidenceWorkflowContract -v
  ```

  Expected: tests fail because the package-smoke evidence remains declared as
  an allowed gap and no producer is a required dependency.

- [ ] **Step 3: Implement the atomic policy transition.**

  Change the producer job, remove only `package-smoke-log` from the declared
  gap ledger, wire the package artifact into the Ubuntu consumer, and add the
  producer to `required-ci-gate` and the workflow test's required-job list.
  Keep the lifecycle producer unchanged.  Update readiness records to say
  package evidence code is present but hosted exact-SHA proof is pending;
  explicitly retain MOD-310's public-SDK, installed single-player, and save
  acceptance work as open.

- [ ] **Step 4: Regenerate and check rendered readiness docs.**

  Run:

  ```powershell
  python tools/site-data/render_handoff.py
  python tools/site-data/render_handoff.py --check
  python tools/site-data/validate.py
  python -m unittest Tests.Tools.test_site_data_contract -q
  ```

  Expected: generated handoff text matches the source contract and no document
  states that MOD-310 or hosted release evidence is complete.

- [ ] **Step 5: Run end-to-end local contract suites.**

  Run:

  ```powershell
  python -m unittest Tests.Tools.test_module_evidence -q
  python .github/scripts/test-workflow-failure-propagation.py
  python .github/scripts/test-verify-exact-required-gate.py
  wsl.exe -d Ubuntu -u nathan -- bash -lc "cd /mnt/d/SparkEngine-release-canonical && python3 -m unittest Tests.Tools.test_module_evidence -q"
  ```

  Expected: all Windows and WSL module-evidence tests pass; the known absence
  of Linux `node` must be reported separately if the broader workflow fixture
  suite is used.

- [ ] **Step 6: Commit the consumer/gate/docs slice.**

  ```powershell
  git add tools/module-evidence/schema.py tools/module-evidence/evidence-gaps.json .github/workflows/build.yml .github/scripts/test-workflow-failure-propagation.py .github/scripts/test-verify-exact-required-gate.py Tests/Tools/test_module_evidence.py docs/readiness/work-items/00-truth-ci-release.json docs/readiness/work-items/30-game-modules.json docs/readiness/ENGINE_READINESS_HANDOFF.md
  git commit -m "fix(evidence): require packaged FPS smoke proof"
  ```

### Task 5: Verify the actual shipping runtime and record the remaining gate

**Files:**
- Modify only if factual status changed: `docs/readiness/work-items/00-truth-ci-release.json`, `docs/readiness/work-items/30-game-modules.json`, `docs/readiness/ENGINE_READINESS_HANDOFF.md`

- [ ] **Step 1: Run local Shipping proof that is available without publication.**

  Use the existing `build/windows-shipping` `MinSizeRel` binary pair and the
  fixed lifecycle collector command.  Check that the evidence binds the local
  HEAD SHA, exact engine/module digests, and direct post-teardown terminal
  record.  Do not commit generated `build/` evidence.

- [ ] **Step 2: Run diff and focused review gates.**

  Run:

  ```powershell
  git diff --check
  git status --short
  python -m unittest Tests.Tools.test_module_evidence -q
  python .github/scripts/test-workflow-failure-propagation.py
  python .github/scripts/test-verify-exact-required-gate.py
  ```

  Expected: no tracked working-tree changes remain after targeted commits;
  ignored generated evidence may exist only under `build/`.

- [ ] **Step 3: Record the exact hosted-proof boundary.**

  State that local verification proves the producer and parser contracts only.
  The still-required external action is a hosted exact-SHA workflow run through
  `build-windows-shipping` → `module-profile-package-smoke` →
  `module-evidence` → `required-ci-gate`; do not push, trigger, or alter
  branch-protection rules without fresh user confirmation.

## Self-Review

1. The plan creates one artifact identity at the shipping producer, verifies
   it before clean installation, and publishes a package log only after both
   backend runs and uninstall.
2. Every production behavior has an earlier test that fails for a missing
   contract, not a typo or test harness error.
3. The policy transition is atomic: the declared gap disappears only as the
   exact required producer and consumer arrive.
4. The plan never treats a generic SDK compile, local run, or same-workflow
   artifact as proof of MOD-310 completion or CI-120 external attestation.

## Execution Status

Implemented locally under the user-authorized release-readiness goal:
hash-bound Windows Shipping MSI manifests, held-identity clean-install
qualification, strict package-smoke records, exact artifact handoff, Ubuntu
rooted consumption, and required-gate wiring. Local Windows and Ubuntu
contract suites pass, but a hosted exact-SHA run remains required through
`build-windows-shipping` → `module-profile-package-smoke` →
`module-evidence` → `required-ci-gate`. MOD-310 remains open for its
public-SDK-only and installed single-player/save acceptance criteria.
