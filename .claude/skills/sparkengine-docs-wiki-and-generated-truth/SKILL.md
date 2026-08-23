---
name: sparkengine-docs-wiki-and-generated-truth
description: >-
  Documentation truth pipeline for SparkEngine — wiki, README, CLAUDE.md counts, generated API docs,
  site-data/readiness contract, and the publishers/validators that enforce them. TRIGGER when: the user says
  "docs are stale", "wiki out of date", "README says X but code does Y", "update the wiki", "sync-wiki failed",
  "publish-wiki CI failed", "site-data validate failed", "readiness handoff", "AUTO section", "badge counts wrong",
  "validate-prompts failed", "doc drift", "regenerate API docs", or asks whether a public claim about the engine is
  true. DO NOT TRIGGER when: deciding whether a release ships (use sparkengine-change-control-and-release-readiness),
  fixing compiler/CMake/CI build mechanics (use sparkengine-build-ci-and-dependencies), reconstructing how a past
  incident unfolded (use sparkengine-failure-archaeology), or writing code comments/Doxygen for a new feature
  (that belongs to the feature's own subsystem skill).
---

# SparkEngine docs, wiki, and generated truth

This skill is the one home for: README/wiki/handbook drift, generated site-data and the engine-readiness
handoff, documentation publishers (GitHub Wiki, site bundle), prompt/document validation, public claims
about the engine, and every regeneration/check workflow. All commands are repo-relative; run them from the
repository root in Git Bash (they are bash scripts — `sh <script>` also works, PowerShell does not).

**When NOT to use this skill:**

| You are actually doing… | Go to sibling |
|---|---|
| Deciding if a change/release is ready to ship; interpreting readiness gates as a verdict | `sparkengine-change-control-and-release-readiness` |
| Fixing CMake presets, compiler errors, CI build-job mechanics, dependency setup | `sparkengine-build-ci-and-dependencies` |
| Reconstructing the timeline/root cause of a past failure | `sparkengine-failure-archaeology` |
| Live debugging of engine behavior | `sparkengine-debugging-playbook` |
| Editing scene serializers themselves (code, not docs about them) | `sparkengine-editor-scenes-and-reflection` |

## 1. The five layers of documentation truth

Never edit at the wrong layer. Identify which layer a wrong statement lives in, then act only there.

| Layer | What it is | Examples | How to fix drift |
|---|---|---|---|
| **Handwritten claims** | Human-authored prose; the only layer you edit by hand | wiki page bodies, `README.md` prose, `CLAUDE.md` prose, `docs/site/*.json` contract values | Edit the file, then run the relevant check script |
| **Generated truth** | Machine-written from code; hand edits are overwritten or rejected | `<!-- AUTO:* -->`…`<!-- /AUTO:* -->` regions in wiki pages; `docs/api/` (gitignored, see §5); `wiki/reference/*-Index.md`, `File-Tree.md`; `wiki/advanced/Codebase-Statistics.md`; `.github/badges/*.json`; counts in README/CLAUDE.md; `docs/readiness/ENGINE_READINESS_HANDOFF.md` | Re-run the generator (§2). Never hand-edit inside AUTO markers or the handoff |
| **Validation** | Scripts that compare layers 1–2 against code | `docs/*.sh check` modes, `tools/check-wiki-*.sh`, `tools/validate-prompts.sh`, `tools/site-data/validate.py`, `tools/publish-wiki.py --check` | Fix the input the validator names, re-run until exit 0 |
| **CI enforcement** | Workflows that run the validators and block or warn | `publish-wiki.yml` (validate job on PRs), `site-data.yml`, `build.yml` jobs `check-format` + `validate-prompts` (blocking) | Reproduce locally with the same command CI runs (§4) |
| **Publication** | Push-to-`Working`-only jobs that publish validated output | `publish-wiki.yml` publish job → GitHub Wiki (`.wiki.git`, branch `master`); `site-data-publish.yml` → public site-data branch | Never publish manually; fix upstream layer and let CI publish |

Canonical wiki source is the in-repo `wiki/` tree (197 pages, all listed in `wiki/_Sidebar.md`). The GitHub
Wiki is a *flattened publication* built by `tools/publish-wiki.py` — never edit the GitHub Wiki directly.

## 2. Regeneration commands (verified)

```bash
docs/update-all-docs.sh            # everything, in order (~30s)
docs/update-all-docs.sh quick      # skips API docs + flowchart (~10s)
docs/update-all-docs.sh check      # dry-run; exit 1 if anything stale
```

Individual generators (each supports a `check` dry-run mode):

| Command | Regenerates |
|---|---|
| `docs/sync-wiki.sh sync` | AUTO: regions — `component_list`, `system_list`, `test_inventory`, `panel_list`, `stats` (in ECS page, `advanced/Testing.md`, `gameplay-tools/SparkEditor.md`, `Home.md`) |
| `docs/generate-api-docs.sh generate` | `docs/api/` pages + symbol TSV |
| `docs/generate-symbol-index.sh generate` | `wiki/reference/` Symbol/Function/Class/Enum/Macro indexes |
| `docs/generate-file-tree.sh generate` | `wiki/reference/File-Tree.md` |
| `docs/generate-class-hierarchy.sh generate` | class-hierarchy Mermaid pages |
| `docs/generate-flowchart.sh generate` | `wiki/getting-started/Engine-Architecture-Flowchart.md` |
| `docs/update-codebase-stats.sh generate` | `wiki/advanced/Codebase-Statistics.md` |
| `docs/update-readme-badges.sh update` | README counts, `.github/badges/*.json`, AI prompt files |
| `docs/update-context.sh update` | CLAUDE.md hardcoded counts (incl. the game-module count) |
| `python3 tools/site-data/render_handoff.py` | `docs/readiness/ENGINE_READINESS_HANDOFF.md` from the readiness contract |

## 3. Validation commands (verified)

```bash
bash tools/check-wiki-nav.sh              # every wiki page ↔ _Sidebar.md (exit 0 = consistent)
bash tools/check-wiki-quality.sh          # stale hardcoded metrics; --warn-only available
bash docs/sync-wiki.sh check              # exit 1 if AUTO regions stale (verified: exits 1 on this tree)
bash tools/validate-prompts.sh --ci       # .github/prompts referential integrity (CI-blocking)
python3 tools/publish-wiki.py --check     # build+validate flattened wiki in a temp dir
python3 -m unittest discover -s Tests/Tools -p 'test_*.py'   # publisher unit tests (CI runs this)
python3 tools/site-data/validate.py                 # readiness/site contract (also --assets --docs --legal)
python3 tools/site-data/render_handoff.py --check   # fail if tracked handoff differs from contract
tools/validate-all.sh --warn-only          # umbrella: pragma-once, panels, wiki-nav, wiring, bloat, doxygen
```

Portable Python note: on Windows `python3` may be missing or shadowed by the Store stub; use `python` or `py -3` locally. CI runners are ubuntu-24.04 and use `python3`.

## 4. Site-data and readiness handoff

The **contract** (handwritten layer, JSON): `docs/site/content.json`, `docs/site/readiness.json`,
`docs/site/docs-catalog.json`. `tools/site-data/validate.py` is fail-closed and enforces closed vocabularies:

- implementation: `absent | stub | partial | functional | complete`
- verification: `none | unit-tested | integration-tested | system-tested`
- support: `unsupported | experimental | supported | primary`
- release: `blocked | candidate | ready`; gates: `blocked | at-risk | passing | not-evaluated`
- work items: `open | in-progress | blocked | done`, priorities `P0–P3`, defined in `docs/readiness/work-items/*.json`

As of 2026-08-23, `readiness.json` `globalRelease.state` is **`blocked`** ("Pre-release hardening") and
**0 of the 18 gates are passing** (all 18 gate states are `blocked`). Capabilities are tracked on
*independent* dimensions (implementation / verification / support / release) — do not conflate
capability states with gate states, or either with "tested" or "CI-enforced". Canonical counts and
promotion rules live in `sparkengine-change-control-and-release-readiness`. **Do not soften these in
docs or README** — promoting a state is a release decision; this skill only keeps the documents
consistent with whatever the contract says.

- `docs/readiness/ENGINE_READINESS_HANDOFF.md` is **generated** by `tools/site-data/render_handoff.py` from the
  contract. Never hand-edit it; CI runs `render_handoff.py --check` and fails on divergence.
- `tools/site-data/generate.py` builds the exact-commit website bundle (default output `.site-data/`, use
  `--allow-dirty` for a local blocked bundle). It regenerates `docs/api/` itself because that directory is
  gitignored. `site-data.yml` proves determinism by generating twice and comparing; `site-data-publish.yml`
  publishes only from `Working`, only after normalizing evidence across build jobs, and refuses superseded snapshots.

## 5. Decision rules

1. **Find the layer first.** A wrong number inside `<!-- AUTO:* -->` markers, `wiki/reference/`, Codebase-Statistics,
   badges, or the readiness handoff means *run the generator*, not edit the text. A wrong claim in prose means edit
   the prose. A wrong readiness state means the contract JSON — and that is a change-control decision.
2. **Code wins over docs.** When code and docs disagree, verify against source (grep the symbol), fix the doc, and
   record load-bearing corrections in `wiki/advanced/Codebase-Observations.md`.
3. **`docs/api/` is gitignored** (`.gitignore` line for `docs/api/`). Never commit it; a clean checkout will not have
   it — anything that needs it (site-data generation) regenerates it.
4. **Blocking vs advisory CI** — do not upgrade an advisory failure into a doc claim of brokenness, and do not cite an
   advisory pass as proof. Blocking: `check-format`, `validate-prompts`, `build-linux-gcc/clang`, asan/tsan, both
   windows-vs2022 configs, coverage, `clang-tidy`, the annotation-count job, `publish-wiki.yml` validate,
   `site-data.yml`. Advisory (`continue-on-error: true` in `build.yml`): `build-linux-msan`,
   `build-windows-vs2026`, `build-linux-mingw-wine` (workflow_dispatch-only), `build-macos`. Fine print
   (warn-only thresholds inside blocking jobs, etc.): `sparkengine-validation-and-qa` §10 is canonical.
5. **New subsystem ⇒ new wiki page** from `wiki/_Template.md`, added to `wiki/_Sidebar.md`, then
   `docs/sync-wiki.sh sync` — otherwise `check-wiki-nav.sh` and the publish-wiki validate job fail.
6. **Publication is CI-owned.** Both publishers re-check that `Working` HEAD has not advanced before pushing;
   a manual push to the `.wiki.git` repo or the site-data branch will be overwritten.

## 6. Known drift ledger (reconciled 2026-08-23)

| Claim seen in docs | Ground truth in code | Status |
|---|---|---|
| `StageBasedExecutor` exists / honors phase order (`wiki/research/Engine-and-Renderer-Landscape.md` §auto-scheduling) | Deleted. No `StageBasedExecutor` anywhere under `SparkEngine/Source`. Live executor: `Engine/ECS/Systems/PhaseSystemManager.h`, created and pumped by `Core/Lifecycle/GameplayLifecycleShared.cpp`; regression test `Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp`. `Codebase-Observations.md` item 2 records the fix correctly | **stale wiki line** — fix the research page when touched |
| Binary scene format usable | `SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp` deliberately returns "Binary scene serialization is unavailable… use JSON/.sparkscene". Live formats: `.sparkscene`, `.json`, `.scenejson` (`SceneSerializer.cpp`); header constants `SCENE_FILE_MAGIC = 0x53504B53` ('SPKS'), `SCENE_FILE_VERSION = 2` describe the dormant binary header | JSON is the current scene truth |
| "10 game modules" (`CLAUDE.md` architecture section) | 11 directories under `GameModules/` — `SparkGameMMOFPS` is the newest; `Codebase-Statistics.md` already says 11 | run `docs/update-context.sh update` |
| Old `.claude/knowledge` base | Retired 2026-06-08; wiki is the single knowledge store | settled |
| "6199 unit tests across 536 files" and similar counts | Auto-maintained numbers; treat every hardcoded count as generated truth — regenerate, never hand-tune | rule, not a bug |

## 7. Failure modes

- **Piping a check to `tail` fabricates exit 0.** `bash docs/sync-wiki.sh check | tail` reports tail's status.
  Verified: the bare command exits **1** on a stale tree while the piped form showed 0. Use
  `bash docs/sync-wiki.sh check; echo $?` or `${PIPESTATUS[0]}`.
- **Interrupting `sync-wiki.sh check` can leave the wiki mutated.** Check mode edits pages in place, diffs against a
  temp snapshot, then restores. Ctrl-C mid-run skips the restore — `git diff wiki/` afterwards.
- **Editing inside AUTO markers** silently survives until the next `sync` run rewrites it, then your edit vanishes.
- **Hand-editing `ENGINE_READINESS_HANDOFF.md`** fails `render_handoff.py --check` in `site-data.yml`.
- **Wiki page not in `_Sidebar.md`** (or vice versa) fails `check-wiki-nav.sh` and blocks the publish-wiki validate job.
- **Assuming `docs/api/` exists** on a fresh clone — it is gitignored; generate it.
- **Publishing race**: both publish jobs abort with a friendly message if `Working` advanced; a "did nothing" green
  publish run is normal, not a failure.
- **`validate-prompts.sh --ci`** blocks PRs when `.github/prompts` files reference deleted source paths — after
  renames/deletions, run it locally before pushing.

## 8. Provenance and maintenance

Facts verified 2026-08-23 against the working tree (branch `claude/whole-nine-yards-20260823`). Re-verify with:

- Layer inventory / scripts exist: `ls docs/*.sh tools/check-wiki-*.sh tools/validate-prompts.sh tools/publish-wiki.py tools/site-data/`
- StageBasedExecutor still dead: `grep -r StageBasedExecutor SparkEngine/Source SparkEditor/Source` (expect no hits)
- PhaseSystemManager still live: `grep -rl PhaseSystemManager SparkEngine/Source/Core/Lifecycle`
- Scene formats: `grep -n "sparkscene" SparkEditor/Source/SceneSystem/SceneSerializer.cpp SparkEditor/Source/SceneSystem/BinarySceneSerializer.cpp`
- Module count: `ls -d GameModules/*/ | wc -l` vs `grep -n "modules)" CLAUDE.md`
- Readiness state: `grep -A2 '"globalRelease"' docs/site/readiness.json`
- Advisory CI set: `grep -n "continue-on-error" .github/workflows/build.yml`
- Check-mode exit semantics: `bash docs/sync-wiki.sh check; echo $?`
- Publisher gate commands: `grep -n "publish-wiki.py\|check-wiki" .github/workflows/publish-wiki.yml`
