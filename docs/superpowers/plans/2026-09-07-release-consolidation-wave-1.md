# Release Consolidation Wave 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate and verify the fail-closed release acceptance gate on the sole canonical SparkEngine branch while establishing a no-data-loss basis for removing redundant worktrees and branches.

**Architecture:** `claude/stable-v1-release` in `D:/SparkEngine-release-canonical` is the only integration source. Candidate work is consumed as reviewed patch-level changes, never as a merge of an entire divergent history. The release controller can clear a draft only after revalidating the current release, tag, assets, and Required CI result.

**Tech Stack:** Git worktrees, GitHub Actions YAML, Python standard library, unittest, CMake/CTest.

**Spec:** `docs/readiness/work-items/00-truth-ci-release.json` (REL-100, REL-110, and profile-required-gates).

## Global Constraints

- Do not push, publish, change a tag, or call a credentialed external mutation.
- Apply candidates with `git cherry-pick --no-commit <commit>`, inspect the staged diff, run its checks, and create a focused commit.
- Never remove a dirty worktree or one with patch-unique commits.
- Remove a worktree only with `git worktree remove -- <path>`; remove a branch only with `git branch -d <branch>`.
- Keep SEC-120 release-blocking until `check_fuzz_policy.py --ci --require-closure` exits successfully.

---

### Task 1: Record reconciliation evidence

**Files:**
- Create: `work/audit_spark_worktrees.py`
- Create: `work/sparkengine-worktree-audit.jsonl`
- Test: `python work/audit_spark_worktrees.py D:/SparkEngine-release-canonical claude/stable-v1-release work/sparkengine-worktree-audit.jsonl`

**Interfaces:**
- Consumes: `git worktree list --porcelain`, `git status --porcelain=v1`, and `git cherry -v <canonical> HEAD`.
- Produces: one JSONL row per worktree containing `clean`, `ancestor_of_canonical`, and `patch_unique_commits`.

- [x] **Step 1: Run the read-only audit**

```powershell
python work/audit_spark_worktrees.py D:/SparkEngine-release-canonical claude/stable-v1-release work/sparkengine-worktree-audit.jsonl
```

Expected: every worktree is classified without changing its files.

- [x] **Step 2: Apply the cleanup eligibility rule**

```text
eligible = clean == true
        && ancestor_of_canonical == true
        && patch_unique_commits == []
```

Expected: no dirty or unrepresented source enters the cleanup set.

### Task 2: Integrate the release acceptance gate

**Files:**
- Create: `.github/scripts/release-acceptance-gate.py`
- Create: `.github/scripts/test-release-acceptance-gate.py`
- Modify: `.github/workflows/release.yml`
- Test: `.github/scripts/test-release-acceptance-gate.py` and `.github/scripts/test-workflow-failure-propagation.py`

**Interfaces:**
- Consumes: `GH_TOKEN`, `GITHUB_REPOSITORY`, `RELEASE_ID`, `RELEASE_TAG`, `TARGET_SHA`, `IS_VERSIONED`, `EXPECTED_ASSETS_FILE`, and `EXPECTED_DIGESTS_FILE`.
- Produces: one `draft: false` API PATCH only after draft release state, exact asset names/digests, tag target, and a successful Required CI Gate are proven.

- [x] **Step 1: Apply and inspect the isolated candidate**

```powershell
git cherry-pick --no-commit 9b33ae8658b387dc75e821c464b6f5030b299452
git diff --cached -- .github/scripts/release-acceptance-gate.py .github/scripts/test-release-acceptance-gate.py .github/workflows/release.yml
```

Expected: bare publication PATCH steps are replaced by one isolated acceptance-gate invocation after existing release safeguards.

- [x] **Step 2: Run the gate's negative-case suite**

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python .github/scripts/test-release-acceptance-gate.py
```

Expected: tag drift, asset-digest tampering, stale CI, duplicate assets, malformed input, and optimization-mode cases fail closed.

- [x] **Step 3: Verify workflow preservation and commit**

```powershell
python .github/scripts/test-workflow-failure-propagation.py
& 'C:\Program Files\Git\bin\bash.exe' tools/check-supply-chain.sh
git diff --cached --check
git commit -m "fix(release): add fail-closed acceptance gate"
```

Expected: all existing stable-v1, exact-CI, canonical-badge, and SEC-120 safeguards remain and the resulting commit is local only.

Completed as reviewed integration commit `8ecc7d1379080c79fe7c0afa18926f255d9f3c3f`
(`fix(ci): harden release acceptance publication gate`), incorporating the isolated
candidate with review corrections. Local re-verification on 2026-09-07 passed
47 acceptance-gate tests and 60 workflow-failure-propagation tests. Windows fixture
execution required Git Bash and a process-local `python3` mapping to installed
Python, since the Windows Store alias was not a usable interpreter. These local
regressions do not prove hosted CI, publication, or stable-v1 readiness.

### Task 3: Remove only proven-redundant worktrees

**Files:**
- Modify: Git worktree metadata and local refs only after Task 1 eligibility is true.
- Test: rerun the Task 1 audit after every cleanup batch.

**Interfaces:**
- Consumes: Task 1 JSONL eligibility fields.
- Produces: fewer worktrees and branches while retaining every dirty or patch-unique source.

- [ ] **Step 1: Remove one eligible worktree at a time**

```powershell
git worktree remove -- "<eligible-worktree-path>"
```

Expected: Git refuses a non-clean target; no force flag is used.

- [ ] **Step 2: Delete only an already-merged local branch**

```powershell
git branch -d "<eligible-local-branch>"
```

Expected: Git refuses a branch whose patches are not in the canonical branch.

### Task 4: Preserve factual release evidence

**Files:**
- Modify: no readiness status until all stable-v1 gates have exact evidence.
- Test: `tools/validate-all.sh` and the SEC-120 closure command.

- [ ] **Step 1: Run the aggregate validator**

```powershell
& 'C:\Program Files\Git\bin\bash.exe' tools/validate-all.sh
```

Expected: record every failure by name; do not suppress or baseline a regression merely to obtain a green result.

- [ ] **Step 2: Demonstrate closure remains release-blocking**

```powershell
python tools/fuzz-policy/check_fuzz_policy.py --ci --require-closure
```

Expected: nonzero until all inventory, target, corpus, and resource-budget closure conditions are satisfied.

## Execution Notes

Inline execution is selected because the user explicitly requested continuous goal-directed release work. Every candidate remains an independently reviewable local commit, and no release-ready claim is allowed until every required stable-v1 profile gate has exact evidence.
