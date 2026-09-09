# RDY-010 Rooted Validator Authority Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Ubuntu module-evidence release consumer use held no-follow authority for every success-relevant repository-relative input.

**Architecture:** Extend `NoFollowDirectoryLease` into a rooted relative-read capability. The Ubuntu gate uses descriptor-relative files and `/proc/self/fd` Git execution; Windows collector behavior remains intact, while Windows positive validation fails closed if rooted Git authority is unavailable.

**Tech Stack:** Python standard library, POSIX `openat` and `O_NOFOLLOW`, Windows `NtCreateFile`, `/proc/self/fd`, `pass_fds`, Git CLI.

**Spec:** `docs/superpowers/specs/2026-09-09-rdy010-module-lifecycle-evidence-design.md`

## Global Constraints

- The release-attesting `module-evidence` job is `ubuntu-24.04`.
- Only a missing fixed final leaf may be downgraded by `--allow-declared-gaps`.
- No HANDLE-derived final pathname may be passed to Git as a capability substitute.
- Use the existing canonical branch/worktree and targeted local commits only.

### Task 1: Rooted relative-reader primitive

**Files:** `tools/module-evidence/strict_json.py`; `Tests/Tools/test_module_evidence.py`.

**Interface:** Add `NoFollowDirectoryLease.read_relative_bytes(relative: str, *, max_bytes: int) -> bytes` and `NoFollowDirectoryLease.posix_git_cwd() -> tuple[str, tuple[int, ...]]`.

- [ ] Write a red test that renames the root after lease acquisition and reads `build/module-evidence/module-targets.json` from the held root.
- [ ] Write a red test that changes a relative intermediate directory to a symlink/reparse point and expects `NoFollowAuthorityError`.
- [ ] Run `python -m unittest Tests.Tools.test_module_evidence.TestLifecycleIsRuntimeProof -v`; expect missing rooted-reader methods.
- [ ] Implement relative component validation that rejects empty, `.`, `..`, backslash, NUL, colon, and absolute inputs.
- [ ] Implement POSIX `openat` and Windows rooted `NtCreateFile` child traversal, final-file snapshot/read/re-snapshot, and no pathname reopen.
- [ ] Implement `posix_git_cwd` as `("/proc/self/fd/<final-fd>", (<final-fd>,))`; raise `NoFollowAuthorityError` outside POSIX `/proc/self/fd`.
- [ ] Re-run the focused class; expect all rooted-reader tests green.
- [ ] Commit with `git add tools/module-evidence/strict_json.py Tests/Tools/test_module_evidence.py` and `git commit -m "feat(evidence): add rooted validator reader"`.

### Task 2: Root target and artifact evidence reads

**Files:** `tools/module-evidence/targets.py`; `tools/module-evidence/artifacts.py`; `tools/module-evidence/validate_manifest.py`; `Tests/Tools/test_module_evidence.py`.

**Interface:** Add `targets.load_target_index_bytes(data: bytes, origin: str) -> dict[str, Any]` and `artifacts.validate_artifact_bytes(data: bytes, leaf_name: str, evidence_type: str, module_name: str) -> list[str]`.

- [ ] Write a red target-index replacement test proving an attacker JSON cannot satisfy the validator after root acquisition.
- [ ] Write a red JUnit/package artifact reparse test proving semantic validation consumes held bytes.
- [ ] Run `python -m unittest Tests.Tools.test_module_evidence.TestTargetContainment Tests.Tools.test_module_evidence.TestArtifactSemanticValidation -v`; expect path-following failures.
- [ ] Parse target JSON from decoded held bytes under existing limits.
- [ ] Parse JUnit XML with expat `Parse(data, True)` and package logs from bytes; do not create temp files or call `Path.read_text`.
- [ ] Map only final target-index leaf absence to `TargetEvidenceUnavailable`; map parse/reparse/ancestor errors to fatal `TargetEvidenceRejected` before gap handling.
- [ ] Run `python -m unittest Tests.Tools.test_module_evidence -q`; expect all evidence semantics and new replacement tests green.
- [ ] Commit with `git add tools/module-evidence/targets.py tools/module-evidence/artifacts.py tools/module-evidence/validate_manifest.py Tests/Tools/test_module_evidence.py` and `git commit -m "fix(evidence): root produced evidence consumers"`.

### Task 3: Root contract, source, and Git inputs on Ubuntu

**Files:** `tools/module-evidence/schema.py`; `tools/module-evidence/paths.py`; `tools/module-evidence/provenance.py`; `tools/module-evidence/lifecycle.py`; `tools/module-evidence/validate_manifest.py`; `Tests/Tools/test_module_evidence.py`.

**Interface:** Rooted registry loaders take `NoFollowDirectoryLease`; rooted Git runs `git -C /proc/self/fd/<fd>` with `pass_fds=(fd,)`; rooted source checks list/open each exact component through the held root.

- [ ] Write red tests for swapped `docs/site/readiness.json`, swapped work-item directory, source-directory reparse, and `git` HEAD after root rename.
- [ ] Run `python -m unittest Tests.Tools.test_module_evidence -v`; expect the existing `repo_root` strings to follow the substitutions.
- [ ] Load `docs/site/readiness.json` and every `docs/readiness/work-items/*.json` through rooted bytes, not `Path` strings.
- [ ] Validate `GameModules/<module>/Source` by descriptor-relative directory listing/opening with exact case and no reparse points; do not call `Path.resolve`.
- [ ] Implement rooted POSIX Git commands with `subprocess.run(["git", "-C", cwd, ...], pass_fds=pass_fds, capture_output=True, text=True, timeout=30, check=False)`.
- [ ] Thread root authority into `ManifestValidator`; replace success-path root string reads with rooted registry/source/artifact/Git operations.
- [ ] Run `python -m unittest Tests.Tools.test_module_evidence -q`; expect all rooted substitution tests green.
- [ ] Commit with `git add tools/module-evidence/schema.py tools/module-evidence/paths.py tools/module-evidence/provenance.py tools/module-evidence/lifecycle.py tools/module-evidence/validate_manifest.py Tests/Tools/test_module_evidence.py` and `git commit -m "fix(evidence): root validator control-plane reads"`.

### Task 4: Enforce the positive-authority platform contract

**Files:** `.github/workflows/build.yml`; `tools/module-evidence/validate_manifest.py`; `docs/superpowers/specs/2026-09-09-rdy010-module-lifecycle-evidence-design.md`; `docs/superpowers/plans/2026-09-09-rdy010-module-lifecycle-evidence.md`; `docs/readiness/work-items/00-truth-ci-release.json`; `docs/readiness/ENGINE_READINESS_HANDOFF.md`; `Tests/Tools/test_module_evidence.py`.

- [ ] Write a red workflow test asserting `module-evidence` runs on `ubuntu-24.04`.
- [ ] Write a Windows-only red test expecting non-policy positive validation to return `1` when descriptor-rooted Git authority is unavailable.
- [ ] Require `posix_git_cwd()` before the validator can print `OK`; preserve Windows collector and `--policy-only` behavior.
- [ ] Document Ubuntu as sole positive authority, Windows as collector/policy-only consumer, and remove all claims that a final HANDLE path secures Git.
- [ ] Run `python -m unittest Tests.Tools.test_module_evidence -q`, `python tools/site-data/render_handoff.py`, `python tools/site-data/render_handoff.py --check`, `python tools/site-data/validate.py`, and `python -m unittest Tests.Tools.test_site_data_contract -q`.
- [ ] Commit with targeted paths and message `fix(evidence): require rooted release authority`.

## Self-Review

1. Task 1 supplies the rooted primitive; Task 2 protects produced evidence; Task 3 protects contract/source/Git inputs; Task 4 enforces the platform boundary.
2. Every task has concrete red tests, commands, files, interfaces, and a targeted commit.
3. No task substitutes a path derived from a Windows HANDLE for rooted authority.

## Execution Handoff

Plan saved to `docs/superpowers/plans/2026-09-09-rdy010-rooted-validator-authority.md`. Execute inline in this canonical checkout with review checkpoints because the previous implementation worker exhausted its quota.
