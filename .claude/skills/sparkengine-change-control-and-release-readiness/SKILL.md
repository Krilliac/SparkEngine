---
name: sparkengine-change-control-and-release-readiness
description: >-
  Canonical runbook for SparkEngine's release-readiness contract: the 18 release gates,
  55 tracked work items, capability truth ledger, claim-promotion rules, same-SHA evidence
  discipline, and the caller/registration/tick/teardown wiring audit. TRIGGER when:
  "is SparkEngine / this subsystem / this module release-ready or production-ready",
  "what's blocking the release", "which readiness work item should I pick up next",
  "how do I update a work item / gate / capability status", "how do I add a new RDY/CI/MOD
  work item", "the handoff or readiness.json is stale", "site-data-validate failed",
  "can I claim this feature is done/stable", "why does the to-do count say the code is clean",
  "what evidence do I need to promote a claim". DO NOT TRIGGER when: you are fixing a
  compiler/CMake/CI build break (use sparkengine-build-ci-and-dependencies),
  launching or operating built binaries (use sparkengine-run-package-and-release),
  debugging a runtime crash (use sparkengine-debugging-playbook), or doing the
  EngineContext DLL injection work specifically (use
  sparkengine-modules-sdk-abi-and-hot-reload).
---

# SparkEngine: Change Control and Release Readiness

Single canonical home for: release gates, readiness status, claim promotion, the
work-item ledger, same-SHA evidence rules, and the "is it actually wired in?" audit.
Verified 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `34ee7ab7`/`0e1fe7e7`).

Term key (defined once):

- **Readiness contract** — the machine-validated JSON that owns every public claim:
  `docs/site/readiness.json` + `docs/readiness/work-items/*.json` + `docs/site/content.json`
  + `docs/site/docs-catalog.json`.
- **Gate** — a release condition (`G00`–`G17`) in `readiness.json` with state
  `blocked | at-risk | passing | not-evaluated`.
- **Capability** — a public-facing feature row (e.g. `rendering.d3d11`) with four
  *independent* dimensions: implementation, verification, support, release.
- **Work item** — a tracked unit of readiness work (e.g. `RDY-000`, `NET-100`) in
  `docs/readiness/work-items/*.json`, status `open | in-progress | blocked | done`.
- **Handoff** — `docs/readiness/ENGINE_READINESS_HANDOFF.md`, **generated** from the
  contract by `tools/site-data/render_handoff.py`. Never hand-edit it (line 1 says so).
- **Same-SHA evidence** — CI artifacts, tests, docs, readiness status, and website
  wording all attached to the exact commit being claimed.
- **Promotion** — moving any public claim upward (a status, a "stable", a number).

## Current state (as of 2026-08-23, working tree — volatile, re-verify)

| Fact | Value |
|---|---|
| Global release state | `blocked` — "Pre-release hardening" |
| Gates | 18 total, **all 18 blocked**, 0 passing |
| Capabilities tracked | 22 (best are `candidate`; none `ready`) |
| Work items | 55 total: 50 open, 2 in-progress (`RDY-000`, `DOC-400`), 3 blocked (`REL-200`, `PLT-250`, `MOD-390`) |
| Unfinished release blockers | 46 (items with `"blocking": true` and status ≠ done) |
| First unblocked item | `RDY-000` — Establish the release profiles and capability ledger |

Re-derive these numbers at any commit (verified command; run from the repo root):

```bash
python - <<'EOF'
import json, glob
items = [i for f in sorted(glob.glob('docs/readiness/work-items/*.json'))
         for i in json.load(open(f))['workItems']]
print(len(items), "items;",
      sum(1 for i in items if i['blocking'] and i['status'] != 'done'), "unfinished blockers")
for i in items:
    if i['status'] == 'in-progress':
        print('in-progress:', i['id'], '-', i['title'])
EOF
```

> **Portable Python note:** on Windows, `python3` may resolve to the Microsoft Store
> stub and fail ("Python was not found"). Use `python` or `py -3` locally; CI runners
> are Linux and use `python3` as written in the workflows.

## The canonical problem: implemented ≠ production-complete

SparkEngine's code is feature-rich and its hygiene signals look clean, but the release
contract says 0/18 gates pass. Three signals actively mislead — never quote them as
readiness evidence:

1. **Source annotation-marker count.** `grep -rn 'TOD[O]\|FIXM[E]\|HACK\|XX[X]'
   SparkEngine/Source SparkEditor/Source GameModules --include='*.h'
   --include='*.cpp' | wc -l` (bracketed classes so this file doesn't match its
   own scanner; functionally identical) returns **6** (verified 2026-08-23), far
   under the annotation-count CI job's warning-only threshold of 20 (job id:
   `to-do-count` spelled without the inner hyphen). Meanwhile 46 blocking
   readiness items are open. Marker comments measure annotation habits, not
   completeness.
2. **Status prose.** `docs/status/PROJECT_STATUS.md` labels most subsystems
   "**Stable** — Production-ready". Gate `G00` explicitly cites that file as a
   "Stale status snapshot". The contract, not the prose, owns public status.
3. **Test/mirror counts.** Promotion rule 3 (below): a test that compiles a copied
   subset, a mock, or a tautology cannot promote readiness — only tests executing
   production source or packaged binaries count (`RDY-010` exists to fix this).

**Rule: any "is X ready?" answer resolves through `docs/site/readiness.json`.**
The weakest of a capability's four dimensions controls public framing.

## Promotion rules (verbatim from `readiness.json` → `statusPromotionRules`)

1. Implementation, verification, support, and release are independent dimensions; never infer one from another.
2. A capability cannot be release-ready while a required gate is not passing or a blocking work item is open.
3. A passing test must execute production source or a packaged binary; mirror-only, tautological, or mock-only tests cannot promote readiness.
4. Every public numeric claim resolves through a generated metric with an evidence path.
5. Every readiness promotion includes source, tests, CI evidence, documentation, limitations, and website impact in the same change.
6. Production-ready wording is forbidden until the global release state is ready at the displayed commit.
7. If live publication fails, the website labels the last valid bundle stale or unavailable; it never silently calls the fallback current.

Corollaries you will actually use:

- Never write "production-ready", "stable release", or a bare test/LOC count into
  README/wiki/website copy unless it is generated from the contract.
- Never close a work item from comments or screenshots; execute its `commands` array
  and attach exact-SHA CI evidence.
- Never weaken a gate to make it pass. Fix the implementation, the evidence path, or
  the public claim.

## File map (all paths verified at HEAD)

| Path | Role | Edit by hand? |
|---|---|---|
| `docs/site/readiness.json` | Gates, capabilities, global release state, promotion rules, execution order | yes — this is source of truth |
| `docs/readiness/work-items/00-truth-ci-release.json` | Wave-0/1 items (10) | yes |
| `docs/readiness/work-items/10-security-network-operations.json` | Security/net/ops items (9) | yes |
| `docs/readiness/work-items/20-platform-runtime-editor.json` | Platform/runtime/editor items (19) | yes |
| `docs/readiness/work-items/30-game-modules.json` | Module items (13) + `parityDimensions` | yes |
| `docs/readiness/work-items/40-installer-governance-docs.json` | Installer/governance/docs items (4) | yes |
| `docs/site/content.json`, `docs/site/docs-catalog.json` | Website copy + docs catalog contract | yes |
| `docs/readiness/ENGINE_READINESS_HANDOFF.md` | Generated handoff (dependency-ordered briefs) | **no — regenerate** |
| `tools/site-data/validate.py` | Fail-closed contract validation | code |
| `tools/site-data/generate.py` | Exact-commit website bundle generator | code |
| `tools/site-data/render_handoff.py` | Handoff renderer (`--check` = staleness gate) | code |
| `tools/site-data/common.py` | Schema version (1), `METRIC_IDS`, contract loader | code |

Work-item schema (every field required by `validate.py`): `id`, `title`, `priority`
(P0–P3), `wave`, `area`, `status`, `owner`, `blocking`, `dependencies`, `parallelWith`,
`rationale`, `sourceContext`, `entryPoints`, `implementationScope`, `acceptanceCriteria`,
`commands`, `testSelectors`, `requiredCiJobs`, `performanceBudgets`,
`documentationUpdates`, `readinessChanges`, `websiteImpact`, `risks`, `outOfScope`,
`definitionOfDone`. Validation is fail-closed: every referenced path must exist unless
listed in `FUTURE_ACCEPTANCE_PATHS` inside `validate.py` (deliberate outputs of
unfinished items).

## Runbook 1 — Answer "what is the status of X?"

Run from the repo root:

```bash
# Human-readable: verdict, gate ledger, capability ledger, wave tables
sed -n '6,15p' docs/readiness/ENGINE_READINESS_HANDOFF.md   # current verdict block
# Machine-readable, one capability:
python -c "import json; d=json.load(open('docs/site/readiness.json')); \
print(json.dumps([c for c in d['capabilities'] if c['id']=='rendering.d3d11'][0], indent=1))"
```

Report all four dimensions plus `limitations` and `blockingWorkItemIds`. Example
correct phrasing: "D3D11 is implementation=complete, verification=integration-tested,
support=primary, **release=candidate** — blocked by `RHI-210` and `PERF-100` behind
gates G02/G03/G04/G09/G14." Wrong phrasing: "D3D11 is production-ready."

## Runbook 2 — Pick up and progress a work item

1. Open the handoff and confirm the item's dependencies are `done` (Wave tables show
   `Depends on`). Do not skip ahead. Currently only `RDY-000` is unblocked.
2. Set its `status` to `in-progress` in the owning `docs/readiness/work-items/*.json`
   file (and set `owner` if you have one).
3. Do the work, keeping **one change** containing: implementation, production-source
   tests, wiki/docs updates (the item's `documentationUpdates` list), readiness JSON
   status/evidence, regenerated handoff, and website-wording impact.
4. Before closing: run the item's `commands` array verbatim and get exact-SHA CI
   evidence (the item's `requiredCiJobs`). Comments/screenshots do not close items.
5. Regenerate + validate (Runbook 4), commit everything together.
6. If acceptance reveals a new blocker, add a new work item (stable ID, `blocking`,
   `dependencies`, full schema above) *before* expanding scope.

## Runbook 3 — Caller / registration / tick / teardown audit

Run this before claiming any system or feature "done". A system that exists but is
never initialized, ticked, or torn down is worse than absent (CLAUDE.md: wire it in
or delete it).

```bash
# from the repo root
bash tools/check-wiring.sh              # Initialize()/Update()/Shutdown() reachable from startup/main loop
bash tools/check-test-registration.sh   # every Tests/Test*.cpp referenced in Tests/CMakeLists.txt
bash tools/check-editor-panels.sh       # every panel registered in EditorPanelFactory
bash tools/validate-all.sh              # all checks; add --warn-only for report-only
```

Checklist per new/changed system:

- [ ] **Caller** — `Initialize()` invoked in the startup path (`SparkEngine.cpp` /
  `EngineContext` / `EditorApplication`), not just defined.
- [ ] **Registration** — registered via `EngineContext` `RegisterSystem<T>()` /
  `RegisterSubsystem<T>()`; panels in `EditorPanelFactory`; tests in `Tests/CMakeLists.txt`.
- [ ] **Tick** — `Update()`/`ProcessCommands()` appears in the main loop.
- [ ] **Teardown** — shutdown/unload path is real. Clean module/resource teardown is
  gate `G10` territory and tracked as open item `LIFE-200` — if your teardown is
  unproven, say so and reference `LIFE-200`; do not claim closure.

Known limitation: `check-wiring.sh` scans a fixed call-site list (engine + editor
cores); it does not prove GameModule DLL wiring. Module lifecycle proof is `RDY-010`.

## Runbook 4 — Validate, regenerate, and keep evidence same-SHA

Local loop (all verified on Windows Git Bash, 2026-08-23; run from the repo root —
`python` / `py -3` on Windows, `python3` on Linux CI):

```bash
python tools/site-data/validate.py            # "Validated 22 capabilities, 18 gates, and 55 work items."
python tools/site-data/validate.py --assets
python tools/site-data/validate.py --docs
python tools/site-data/validate.py --legal
python tools/site-data/validate.py --modules
python tools/site-data/render_handoff.py            # regenerate handoff after JSON edits
python tools/site-data/render_handoff.py --check    # exit 0 iff tracked handoff is current
git diff --exit-code                                # clean regeneration ⇒ no surprise diff
```

Fail-closed guard (verified): `python tools/site-data/validate.py --require-ready`
exits 1 today with `globalRelease.state: release is not ready`. That is the intended
behavior — it becomes the release gate, not something to bypass.

Full bundle generation:

```bash
python tools/site-data/generate.py --output "$TMP/site-data" --allow-dirty --skip-doc-health
```

Caveats (both verified 2026-08-23): generation **refuses a dirty tree** without
`--allow-dirty` ("exact-commit publication refused"), and a dirty-tree bundle is
marked `blocked`, never `current`. On this Windows machine the API-doc step exceeds
its internal 240 s timeout and generation fails — treat full generation as a CI
concern: the `site-data-validate` job (`.github/workflows/site-data.yml`) runs it
twice with pinned inputs and `diff --recursive` to prove determinism on every push/PR.

## CI wiring you must not misread

- `.github/workflows/site-data.yml` — job `site-data-validate`: validates the
  contract, checks handoff freshness, proves deterministic double generation.
  Runs on push to `Working` and PRs. If it is red, your readiness JSON, handoff,
  or referenced paths are broken.
- `.github/workflows/site-data-publish.yml` — publishes the exact-commit bundle
  after "Build SparkEngine" completes on `Working`; it normalizes per-job evidence
  via the GitHub API. Failed publication ⇒ site shows stale/blocked, by design.
- `.github/workflows/build.yml` — advisory lanes exist: `build-linux-msan`,
  `build-windows-vs2026`, `build-linux-mingw-wine` (workflow_dispatch only), and
  `build-macos` are job-level `continue-on-error: true`; the annotation-count job
  only warns above 20. (`clang-tidy` is a **blocking** job — warnings inside it are
  advisory, but configure/compile failures block; canonical gate fine print:
  `sparkengine-validation-and-qa` §10.) **A green run does not certify the advisory
  surfaces** — repairing fail-closed CI is open item `CI-100` (gate `G01`).
  Do not present an advisory-lane pass or the overall green check as support-gate
  evidence (promotion would violate rules 2 and 5).

## When NOT to use this skill

- Build/compile/CMake/CI-reproduction problems → `sparkengine-build-ci-and-dependencies`
  (plus `wiki/development/CI-Reproducible-Builds.md`).
- Running/operating built binaries, logs, crash-dump locations →
  `sparkengine-run-package-and-release`.
- Diagnosing a runtime bug or crash → `sparkengine-debugging-playbook`
  (and `sparkengine-failure-archaeology` for past incidents).
- EngineContext cross-DLL injection, module ABI, hot reload →
  `sparkengine-modules-sdk-abi-and-hot-reload`.
- Architectural invariants (service locator, ownership, layering) →
  `sparkengine-architecture-contract`.

Use *this* skill whenever a change is about to alter what the project publicly
claims, close a work item, touch a gate, or assert readiness.

## Provenance and maintenance

Written 2026-08-23 against the working tree (branch
`claude/whole-nine-yards-20260823`, uncommitted changes ahead of `34ee7ab7`) by
inspecting the live contract, tools, and workflows — not a full-suite or CI run at
this exact tree. All counts and states above are volatile. Re-verify with:

```bash
git status --short && git rev-parse HEAD                                      # note the exact tree you re-verify against
python tools/site-data/validate.py                                            # expect "22 capabilities, 18 gates, 55 work items" (counts may grow)
python tools/site-data/render_handoff.py --check                              # handoff currency
python -c "import json; print(json.load(open('docs/site/readiness.json'))['globalRelease']['state'])"   # expect "blocked" until REL-200 closes
grep -rn 'TOD[O]\|FIXM[E]\|HACK\|XX[X]' SparkEngine/Source SparkEditor/Source GameModules --include='*.h' --include='*.cpp' | wc -l   # was 6
grep -n "continue-on-error: true" .github/workflows/build.yml                 # advisory-lane list
sed -n '6,15p' docs/readiness/ENGINE_READINESS_HANDOFF.md                     # current verdict block
```

If any re-verification disagrees with this file, the contract wins — update this
skill, never the other way around.
