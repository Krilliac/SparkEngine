# Session Handoff — Release-Readiness Run (2026-09-24 → 2026-09-27)

> **Audience:** Programmers (the next session, human or AI)
>
> **Thread Context:** Mixed (process page; no runtime code)
>
> **Platform/Backend Scope:** Work was built and tested on Linux GCC 13 only. Nothing from this run has been compiled with MSVC.

## Overview

This run implemented release-readiness slices on branch `claude/release-readiness-ultracode-agents-lofqcu` (PR #587) with parallel agents in one shared checkout. It landed about 200 commits between 2026-09-24 and 2026-09-27. The branch is **not behind `Working`** and is fully pushed. No hosted CI has run on it; every result below is local.

The authoritative per-criterion state is the readiness ledger (`docs/readiness/work-items/*.json`). The generated summary is `docs/readiness/ENGINE_READINESS_HANDOFF.md`. This page is the narrative: what landed, how it was checked, what is left, and what went wrong in the process.

## When to Use

- You are the first session after this run, most likely on a Windows machine with MSVC.
- You need to know what "implemented" means right now, and what has never been run on Windows.
- You are about to run multi-agent work in one checkout again; read [Process Lessons](#process-lessons) first.

## State at Handoff

| Measure | Value |
|---------|-------|
| Readiness criteria implemented / total | 46 / 259 (0 evidenced) |
| Work items | 0 done, 51 in progress, 5 blocked, 8 open |
| Linux GCC Release build (`linux-gcc-release`, LTO off) | Full build succeeds |
| CTest, full run before the last fixes | 247 / 257 passed |
| CTest after fixes | All 10 failing lanes pass on rerun (OpenGL lanes need `xvfb-run`); the full 257-test suite was not rerun afterwards |
| Windows MSVC build | **Never built from this run** |

"Implemented" means production code plus a registered test at HEAD. "Evidenced" needs an exact-commit hosted CI run (`ci:<workflow>/<run>@<sha>`), and there are none. The ledger reconciliation (`10a0a8b`) checked all 64 items against HEAD.

## What Landed (by work item)

The commit subject names the work item; list a slice with `git log --oneline origin/Working..HEAD --grep <ID>`. Highlights:

- **Runtime and platform:**
  - **RDY-015:** Linux `-require-game` and a post-teardown `SPARK_MODULE_LIFECYCLE` record. With `bde4105`, the script engine is created before modules load on every host, so all ten `ExperimentalModuleLifecycle_*` lanes pass locally.
  - **PLT-210:** module content is staged next to the engine executable, and windowed startup refuses to continue when the render device fails.
  - **EDT-210:** `-scene` runs on the Linux host; the editor executable smoke test runs under Xvfb.
  - **HEAD-220:** bounded `-exec` scripts, and an opt-in 10-minute FPS NullRHI soak (`SPARK_ENABLE_SOAK_TESTS`).
  - **LIFE-200 / OD-01:** the Windows hosts no longer call the deleted `RegisterCoreSubsystems`. That was a Windows-only build break found by reading, not by compiling.
- **Persistence:**
  - **DATA-120:** TFDatabase backup/restore and a crash-recovery drill.
  - **SAVE-230:** an `AtomicWrite_` interruption rehearsal, reflected-scene validation diagnostics and on-disk fixtures.
  - **MOD-340:** Platformer progress survives a relaunch.
  - **MOD-380:** race results survive a SaveSystem restart.
- **Rendering:**
  - **RHI-230:** the shipped GLSL shaders are compiled to SPIR-V, with Vulkan goldens on Lavapipe.
  - **RHI-240:** OpenGL goldens on llvmpipe, and the device row is classified independently of GLDevice.
  - **ENG-220:** glTF skeletons and clips are imported into AnimationManager, the asset reference-closure validator is added, and the Linux DirectXMath stub gets a true slerp.
- **Networking and security:**
  - **NET-100:** protocol-version negotiation.
  - **MOD-315:** FPS multiplayer runs on NetworkManager handlers, with a three-process LAN convergence harness and hostile-input rejection.
  - **SEC-100:** security-runtime and network-integration CI lanes.
  - **SEC-110:** the supply-chain checker accepts only symlinks tracked by the pinned submodule commit.
- **Scripting:**
  - **ENG-200:** real physics and event script bindings, plus engine-owned collision and trigger dispatch to scripts.
  - **MOD-390:** engine-owned `.vscript` graph I/O, and the shipped scripts run headless to the win objective.
- **Game modules:**
  - **MOD-300:** exact quickload state.
  - **MOD-320:** exact-case area scenes; unauthored dungeons fail closed.
  - **MOD-330:** the ARPG hero, monsters and props live in the ECS World.
  - **MOD-350:** RPG NPCs route through NavMesh.
  - **MOD-360:** a fail-closed asset-reference check for OpenWorld.
  - **MOD-370:** unattached RTS behavior trees removed.
  - **MOD-380:** Racing drives on Jolt vehicles, with barrier and checkpoint bodies.
- **Assets:** Blender kits for the discovered game modules (`Art/Blender/<Module>/`, exported to `Assets/Models/<Short>/Kit/`) with provenance. Procedurally composed WAV music ships for Platformer, Racing, RPG and RTS (`tools/audio/compose_module_music.py`).
- **CI and tooling:**
  - **CI-110:** a per-check clang-tidy diagnostic budget ratchet.
  - **CI-120:** documented build commands are checked against the presets, and a strict dependency closure is derived from `dependencies.lock`.
  - **REL-100:** stable tag and changelog helpers.
  - **BLD-100:** a two-tree reproducibility comparator.
  - **Fuzzing:** fuzzing moved to `FuzzerTests/` with a minimized generated corpus.
  - **Builds:** faster local builds (LTO off, ccache, mold, a SparkTests precompiled header).

## Known Gaps Committed On Purpose

These were committed because the owner asked for everything to be committed. They need attention first:

| Commit | Item | Gap |
|--------|------|-----|
| `b2d2953` | DATA-120 | The operation-id idempotency ledger has **no caller**: unlock purchases persist through a debounced sweep with no per-request id. It has since been removed and TFDatabase is back to schema v2; a v3 file with an empty ledger still loads (see `docs/specs/persistence.md`). |
| `14d56eb` | REL-190 | The `profile-required-gates` job in `release.yml` was mid-edit when the session restarted. Review it before trusting it. |
| `445f240`, `11058e2`, `3ff83ad`, `3d1adfc` | ENG-220, SDK-240, SAVE-230, MOD-390 | Started by readiness runs and interrupted (usage limit or restart) before an adversarial review. They build and their tests pass on Linux. |
| `a4b152e` | SEC-120 | `VisualScriptGraphIO` (a `.vscript` JSON parser) is covered by the ENG-200 `Engine/Scripting` subtree exemption. Decide whether it should get its own SEC-120 parser entry. |

## Test Status

The full CTest run before the final fixes passed 247 of 257. The ten failures and their outcome:

| Lane | Cause | Outcome |
|------|-------|---------|
| FuzzPolicy, FuzzPolicyAdversarial | New parser files not classified | Fixed, `a4b152e` |
| SparkNetworkBoundaryStatic | `FuzzerTests/` not treated as non-shipped after the move | Fixed, `9ebd5de` |
| ModuleManifest_Contract | Stale `FPSScene_` count pin | Fixed, `7c59c28` |
| ExperimentalModuleLifecycle_SparkGameVisualScript | Script engine created after modules load | Fixed, `bde4105` |
| readiness-cross-references, runtime-bundle-validation | Ledger listed now-existing CI jobs and selectors as planned | Fixed, `10a0a8b` |
| site-data-contract | Took ~13 min against a 300 s timeout: every in-process validation re-lexed module sources, re-resolved 1200+ manifest entries per asset root and re-ran shlex over every documented command | Fixed, `6193f62` (memoized scans; now 180 s) |
| `docs/update-all-docs.sh` (symbol indexes) | The symbol TSV reader used csv quoting, so a brief starting with `"` lost its quotes and failed the source-projection check | Fixed in the final docs commit (`tools/docs_contract.py` reads with `QUOTE_NONE`; regression test in `test_docs_health.py`) |
| SparkOpenGLTests, SparkOpenGLGoldenTests | No `DISPLAY` in the CTest environment | Environment only; both pass under `xvfb-run -a` |

## What Is Left (in order)

1. **Windows MSVC build and tests.** Configure `windows-release` (and `windows-debug`), build everything and run CTest. Expect Windows-only breaks: OD-01 was one, and the `bde4105` Windows host changes were never compiled.
2. **Hosted CI on PR #587 at one commit.** Fix what fails, then promote criteria to `evidenced` with `ci:` references. Hosted CI is the only way to get evidence.
3. **Remaining code work:** the remaining unmet criteria are listed by need in the reconciliation commit (`10a0a8b`) and the handoff. The largest code items:
   - **NET-100:** the SecureChannel is not wired into NetworkManager, and the plaintext login field is still sent.
   - **NET-110.**
   - **SEC-110/120.**
   - **TF-110/120.**
   - **LIFE-200:** the Linux headless lifecycle loop.
   - **EDT-210:** asset drag-and-drop and the cook scenario.
   - **SAVE-230:** prefab and editor-state work.
   - **INST-130/131/132.**
   - **MOD-320:** timestamp-based character IDs.
4. **Slices that were queued but never ran** (both relaunched readiness runs were lost to a restart):
   - **Scripting:** MOD-390 real `playSound`/`playAnimation` bindings; ENG-200 package-root resolution and the hot-reload state rule.
   - **TF-110:** forged loadout rejection, `net_*` impairment wiring, and the multi-client harness.
   - **Modules:** the MOD-360 player controller, the MOD-300 localization and positioning labels, the MOD-340/360 README boundaries, and three MOD-320 persistence slices.
   - **SDK-240:** the installed-SDK template, and the FPS private-include ratchet (shared with MOD-310).
   - **RDY:** RDY-010 mirror-test retirement, and RDY-015 removal of the mirror smoke files.
   - **SAVE-230:** SceneMigration fixtures (partly in `3ff83ad`).
   - **REL-190:** gating (partly in `14d56eb`).
   - **INST-130:** rollback rebuild, staged install and preflight.
   - **EDT-210:** asset drag and the cook scenario.
   - **Platform and CI:**
     - PLT-200 MSVC toolset derivation;
     - GOV-400 wording selectors;
     - PLT-220 macOS target and preset;
     - OPS-100 selectors and the OPS-110 runbook;
     - PLT-230/240/250;
     - the CI-110 Linux golden baseline.
   - **Others:** the LIFE-200 boot loop, the ASSET-220 installed package smoke, the SEC-100 hostile-frame regressions, the HEAD-220 save/reload test, and the ENG-220 animation-versus-reference check.
5. **Release rehearsal** (REL-190/191/192/193, REL-200) needs owner sign-off, protected publication and independent download verification.
6. **Out of scope by owner decision:** consoles (PLT-250, OD-12). Steam Deck is tracked separately as DECK-100 in `docs/plans/steam-deck-target.md`.

## Process Lessons

- **Never export `GIT_DIR` or `GIT_WORK_TREE` for a command that may run `git init`.** A validator run with both set against a scratch export made `test_validate_ctest_policy.py`'s nested `git init` re-initialise the main repository. It set `core.worktree` to the export, so for about two hours every `git status` read the export. Agents then wrongly decided that four implemented slices were "already in HEAD". Fixed in `2f75a82`; the test now strips `GIT_*`.
- **`git config` and `git submodule init/update` inside a linked `git worktree` write the shared `.git/config`.** Use `git clone --shared --no-checkout <repo> <dir>` for an isolated checkout that has its own config.
- **A clean `git status` is not proof that something is committed.** Check with `git grep HEAD` / `git log -S`. Run `git update-index --really-refresh` before trusting status after heavy agent churn; one agent found a stale stat cache hiding modified files.
- **Workflow resume does not help parallel batches.** The resume cache matches an unchanged prefix of agent calls, and parallel calls start in a different order each run. Relaunch only the unfinished batches instead.
- **Validators that the contract suite runs in-process must stay fast.** `site-data-contract` silently grew past its 300 s timeout as the asset manifest and module sources grew; `6193f62` memoizes the scans. Watch its runtime when adding validation.
- **Never delete from shared scratch with globs.** One integrator's `*.json` cleanup deleted other agents' files.
- **Container restarts kill every background agent.** Commit and push small units often, so a restart loses in-flight work only.
- **Private builds in fresh worktrees miss ccache** when `base_dir` is the main checkout, so each agent paid for a near-full build. On a fast machine prefer one shared build tree behind `tools/build-lock.sh`.

## Troubleshooting

- **A link to a heading that contains backticks fails `validate_docs_links`.** `heading_ids` blanks code spans before slugging, so it never matches GitHub's anchor. Keep linked headings free of code spans (see `b16b711`), or fix `heading_ids` to keep code-span text like GitHub does. After `b16b711`, `docs/update-all-docs.sh check` passes, so the DOC-410 note that the link validator fails at HEAD is out of date.

- **The OpenGL lanes fail locally.** CTest does not inherit a display; run `xvfb-run -a ctest -R OpenGL`.
- **The `Saves/test_*.db` files appear after a SparkTests run from the repo root.** They are ignored now. The TF120 tests should move to a temporary directory.
- **Stray `crash-*` files in the repo root** are libFuzzer reproducers and are ignored now. Minimize them into `FuzzerTests/corpora/` if they are new.

## Related Pages

- [Workflow Patterns](Workflow-Patterns.md): fast rebuilds, the build lock, disk budget.
- [CI Reproducible Builds](CI-Reproducible-Builds.md)
- [Release Publication Stages](Release-Publication-Stages.md)
- [Readiness handoff (generated)](../../docs/readiness/ENGINE_READINESS_HANDOFF.md)

## Source & Freshness

Written 2026-09-27 at the end of the run, from `git log origin/Working..HEAD`, the reconciled ledger (`10a0a8b`), the full CTest log and the agents' integration reports. Replace or archive this page once the Windows build and a hosted CI run have happened.
