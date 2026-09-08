# RDY-000 Documentation-Health Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the isolated documentation-health generator resolve Bash through the existing trusted host-shell policy so it runs on a standard Windows Git installation even when `bash` is absent from `PATH`.

**Architecture:** `tools/site-data/generate.py` already centralizes host-shell resolution in `trusted_bash()` for API documentation generation. The health regeneration path must use that same resolver instead of spelling a bare `bash` executable. A focused mocked regression will exercise the real `documentation_health()` flow through its temporary worktree and assert the bounded process receives the resolved executable.

**Tech Stack:** Python 3 standard library, `unittest.mock`, Git worktrees, Git Bash on Windows.

**Spec:** `docs/readiness/work-items/00-truth-ci-release.json` (`RDY-000`); `tools/site-data/generate.py:117-126,658-713`.

## Global Constraints

- Preserve fail-closed behavior: a missing or unsafe Bash executable must still raise `SiteDataError` rather than publish unknown health.
- Reuse `trusted_bash()`; do not add a second PATH or environment override policy.
- Preserve the isolated detached-worktree lifecycle and 1800-second timeout.
- Add no Python dependency and no platform-specific test skip.
- Run the focused regression before the full site-data contract suite.

---

### Task 1: Lock the shell-resolution contract with a regression

**Files:**

- Modify: `Tests/Tools/test_docs_health.py:738-825`
- Test: `Tests/Tools/test_docs_health.py::PublishedDocumentationHealthTests::test_regeneration_uses_trusted_bash`

**Interfaces:**

- Consumes: `site_generate.documentation_health(commit, emit_to=None)` and `site_generate.trusted_bash() -> str`.
- Produces: a regression proving the isolated generator invokes `run_bounded_process()` with the resolved Bash executable as argument zero.

- [ ] **Step 1: Write the failing test**

```python
    def test_regeneration_uses_trusted_bash(self) -> None:
        observed: list[list[str]] = []

        def write_health(command: list[str], *, cwd: Path, environment: dict[str, str],
                         timeout: int, label: str) -> SimpleNamespace:
            observed.append(command)
            self.assertEqual("C:/trusted/bash.exe", command[0])
            self.assertEqual("update", command[-1])
            self.assertEqual("isolated documentation health check", label)
            Path(environment["SPARK_DOC_HEALTH_OUTPUT"]).write_text(
                json.dumps(self.payload()), encoding="utf-8"
            )
            return SimpleNamespace(returncode=0)

        successful_git = SimpleNamespace(returncode=0, stdout="", stderr="")
        with mock.patch.object(site_generate, "trusted_bash", return_value="C:/trusted/bash.exe"), \
             mock.patch.object(site_generate, "run_bounded_process", side_effect=write_health), \
             mock.patch.object(site_generate.subprocess, "run", side_effect=[successful_git, successful_git]):
            health = site_generate.documentation_health(EXACT_SHA)

        self.assertEqual("current", health["status"])
        self.assertEqual(1, len(observed))
```

- [ ] **Step 2: Run the test to verify the current bare-shell invocation fails it**

Run:

```powershell
py -3 -m pytest Tests\Tools\test_docs_health.py -q
```

Expected: the new assertion fails because `documentation_health()` passes `bash` rather than `C:/trusted/bash.exe` to `run_bounded_process()`.

- [ ] **Step 3: Route the health generator through the existing resolver**

In `tools/site-data/generate.py`, replace the literal command at the `run_bounded_process()` call with this exact command construction:

```python
            result = run_bounded_process(
                [trusted_bash(), str(checkout / script_relative), "update"],
                cwd=checkout,
                environment=environment,
                timeout=DOC_HEALTH_TIMEOUT_SECONDS,
                label="isolated documentation health check",
            )
```

Do not change the surrounding `git worktree add/remove`, evidence-file, or result-summary logic.

- [ ] **Step 4: Run the focused regression and fail-closed health tests**

Run:

```powershell
py -3 -m pytest Tests\Tools\test_docs_health.py -q
```

Expected: every documentation-health test passes, including the new resolver regression and existing missing-evidence/nonzero-exit tests.

- [ ] **Step 5: Commit the self-contained repair**

```powershell
git add -- tools/site-data/generate.py Tests/Tools/test_docs_health.py
git commit -m "fix(site-data): resolve trusted bash for doc health"
```

### Task 2: Verify the release-contract surface

**Files:**

- Verify: `tools/site-data/generate.py`
- Verify: `Tests/Tools/test_site_data_contract.py`
- Verify: `docs/readiness/ENGINE_READINESS_HANDOFF.md`

**Interfaces:**

- Consumes: the resolver-backed `documentation_health()` from Task 1.
- Produces: current validated readiness data without claiming completed release evidence.

- [ ] **Step 1: Run the full readiness-contract regression**

Run:

```powershell
py -3 -m pytest Tests\Tools\test_site_data_contract.py -q
```

Expected: all contract and generated-handoff consistency checks pass.

- [ ] **Step 2: Validate and check the generated handoff**

Run:

```powershell
py -3 tools\site-data\validate.py
py -3 tools\site-data\render_handoff.py --check
git diff --check
```

Expected: validation succeeds, the handoff is current, and Git reports no whitespace errors.

- [ ] **Step 3: Record verification only if the worktree is otherwise clean**

Run:

```powershell
git status --short
git log -1 --oneline
```

Expected: only the Task 1 commit is present; do not broaden the change into unrelated documentation or release-state claims.

## Self-Review

- Spec coverage: Task 1 repairs the only duplicated shell-resolution path; Task 2 verifies the release contract and generated handoff.
- Placeholder scan: no deferred implementation, unnamed tests, or unspecified commands remain.
- Type consistency: `trusted_bash()` remains `() -> str`; the test checks the `list[str]` passed to `run_bounded_process()` without changing its interface.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-09-08-rdy000-documentation-health-shell.md`. This is intentionally separate from the much larger public-SDK module migration and editor recovery/asset certification streams, which need their own approved designs.
