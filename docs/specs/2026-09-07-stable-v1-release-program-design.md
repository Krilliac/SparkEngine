# SparkEngine stable-v1 Release Program — Design

Date: 2026-09-07
Status: Proposed for review

## Purpose

Bring SparkEngine's declared `stable-v1` profile to a state that can be
truthfully called release-ready.  Release readiness means more than a successful
local build: every required release gate must pass for one exact candidate
commit, the generated readiness contract must identify that commit and its
evidence, and a clean installed consumer must survive the supported lifecycle.

This program uses the repository's release ledger as the authoritative work
breakdown, while also investigating and adding any material release blocker
found outside the current ledger.  A gate cannot be promoted based on intention,
an advisory CI result, a stale artifact, or evidence from another commit.

## Release-ready definition

The `stable-v1` profile is release-ready only when all of the following are
true for the same immutable candidate SHA:

1. `docs/site/readiness.json` and the validated work-item corpus report no open
   blocker or required gate for `stable-v1`.
2. Required CI is fail-closed, publishes exact-SHA artifacts, and has current
   enforcement rather than post-hoc advisory status.
3. Reproducible Shipping builds, source/package asset manifests, SDK consumer
   tests, D3D11 and NullRHI runtime evidence, editor round-trip evidence,
   installation/recovery evidence, and the SparkGameFPS installed module slice
   all pass for that candidate.
4. A versioned release artifact has matching version, source SHA, dependency
   lock digest, toolchain/configuration identity, checksums, SBOM, provenance,
   vulnerability/license scan, signature, and protected approval evidence.
5. Clean-machine installation, launch, migration, crash/symbol handling,
   uninstall, upgrade, and rollback rehearsals complete with the evidence
   attached to the candidate/tag.
6. Public documentation, legal/support/security claims, website data, release
   notes, and downloadable artifacts are generated or validated from the same
   contract and do not overstate scope.

Evidence that does not name the candidate SHA, required configuration, command,
and result is insufficient.  A failing, skipped, advisory, or unavailable check
is a blocker unless the contract expressly declares the surface outside
`stable-v1`.

## Scope and release boundary

The release target is the repository-declared `stable-v1` surface: Windows 11
x64, D3D11, the editor, NullRHI, an installed runtime/package, public SDK, and
the installed single-player SparkGameFPS slice.  Experimental, unsupported, and
out-of-profile surfaces remain explicitly labeled; they cannot be represented as
stable release capability.

The program is not limited to the existing item list.  During implementation,
source inspection, mutation testing, package inspection, security review, or
clean-machine rehearsal that exposes a material release defect must either fix
it with test-backed evidence or add a blocking ledger item before readiness can
be promoted.

## Program architecture

The release program has four control planes:

1. **Truth contract.** `docs/readiness/work-items/*.json`,
   `docs/site/readiness.json`, and the site-data validators define release
   profiles, gates, metrics, ownership, dependencies, and public wording.
2. **Execution and evidence.** CMake/CTest, package consumers, deterministic
   asset validators, runtime/editor smoke tests, CI workflows, and release jobs
   produce machine-readable exact-SHA evidence.  The generator validates and
   publishes it; documentation does not hand-copy results.
3. **Artifact integrity.** One authoritative version flows into CMake, SDK,
   installer, launcher, package metadata, checksums, SBOM, attestations, and
   release notes.  Promotion verifies that these all refer to the same source
   SHA and dependency lock.
4. **Release approval.** GitHub enforcement, protected release environments,
   signing identities, and final human approval remain explicit external gates.
   The repository can prepare and verify their contracts, but it must never
   claim they happened when account-owner or credential access is unavailable.

## Delivery waves

The current dependency graph is already grouped into waves.  Each wave must
produce an independently auditable state before dependent gates promote.

### Wave 0 — Truth and fail-closed evidence

`RDY-000`, `RDY-010`, `RDY-020`, `CI-100`, and `DOC-410` establish the sole
readiness contract, production-source test evidence, content/package manifests,
fail-closed CI, and deterministic documentation generation.  This wave is the
first implementation sub-project because every later claim depends on it.

The Blender connector belongs here only as a constrained evidence producer for
the model/content portion of `RDY-020`; it cannot mark a package or release gate
complete by itself.

### Wave 1 — Build, security, and release substrate

`CI-110`, `CI-120`, `BLD-100`, `REL-100`, `REL-110`, `SEC-100`, `SEC-110`,
`SEC-120`, and `OPS-100` provide reproducible Shipping configurations, test and
analysis enforcement, release provenance, supply-chain policy, fuzz boundaries,
runtime security, and crash/symbol operations.

### Wave 2 — Supported Windows product

`PLT-200`, `RHI-210`, `HEAD-220`, `LIFE-200`, `EDT-210`, `ASSET-220`,
`INST-130`, `SAVE-230`, `SDK-240`, and `PERF-100` prove the declared runtime,
renderer, headless behavior, lifecycle, editor, package consumer, installer,
migration, SDK/ABI, and performance budgets on the supported Windows product.

### Waves 4–6 — Installed module, interoperability, and release rehearsal

`MOD-290` and `MOD-310` prove the installed public-SDK module kit and
SparkGameFPS release slice.  `ENG-220` proves canonical D3D11 model import.
`DOC-400`, `GOV-400`, and `REL-200` complete public framing, legal/support
governance, and the signed end-to-end release rehearsal.  `REL-200` is the only
terminal gate; it stays blocked until every preceding required gate is proven.

## Blender asset evidence connector

The repository will use a local, project-scoped stdio MCP server rather than a
general remote-control add-on.  It wraps the existing deterministic model
pipeline and exposes only these operations:

| Operation | Mutates repository state | Contract |
| --- | --- | --- |
| `pipeline_status` | No | Report pinned Blender/Python/tool availability and expected asset inputs. |
| `validate_models` | No | Run the existing confined OBJ/manifest/preview validator and return structured results. |
| `generate_starter_models` | Yes | Invoke the pinned headless generator; writes only the declared model, manifest, lock, and preview paths. |
| `capture_template_evidence` | Yes | Capture only the declared template evidence paths after an explicit engine invocation. |

The server resolves the repository root from its own location, validates every
requested path against explicit allowlists, uses the pinned Blender executable,
and rejects arbitrary Python, shell arguments, URLs, paths, or interactive
scene commands.  Mutating tools require explicit tool confirmation.  The
connector is tested for command construction and path confinement; it adds
repeatable evidence but never bypasses asset/package validation or installed
consumer smoke tests.

## Evidence rules

- Tests must load the production code or installed artifact they claim to test;
  mirrors, tautologies, and repository-only fallbacks do not count.
- CI outcomes are accepted only from the exact source commit, with declared
  required jobs and no `continue-on-error` escape for a required job.
- Asset manifests reject missing, case-mismatched, tampered, traversal, or
  unlicensed/incomplete content according to the contract before package
  assembly.
- Reproducibility checks compare two independent generation/build outputs
  excluding only explicitly declared timestamp fields.
- Performance and visual claims require declared reference environments,
  thresholds, repeatable captures, and retained results.
- Every generated documentation/website datum must validate against source or
  exact evidence.  Marketing wording cannot promote a capability.

## External-authority gates

Some required actions cannot be completed with repository write access alone:

- The GitHub account owner must enable the declared required-check ruleset and
  retain its enforcement evidence.
- Protected release environments, signing identities, and artifact-attestation
  permissions require authorized credential and organization setup.
- A real release tag, GitHub publication, download verification, and any
  clean-machine certification require their designated environments and owners.

For each such action, the repository will supply a reproducible preflight,
expected artifact, validation command, and a narrow hand-off request.  Its
ledger item remains open until authoritative external evidence is attached.

## Testing and promotion strategy

Each implementation task starts with a failing regression or contract test,
then implements the smallest production change to satisfy it.  Local checks
provide fast feedback; required CI and clean-environment runs provide promotion
evidence.  No generated readiness status changes from blocked/open to ready
until its acceptance criteria have all been independently verified.

The final audit walks every `stable-v1` work item, required gate, public claim,
artifact, test selector, CI job, and external evidence requirement.  It verifies
current state, not previous reports.  Only then may `REL-200` be marked complete
and the engine be called release-ready.

## First implementation sub-project

The first plan implements Wave 0 in dependency order:

1. Audit and complete the `RDY-000` truth contract and deterministic generator.
2. Repair/verify `CI-100` failure propagation and exact-SHA gate evidence.
3. Close remaining `RDY-010` production-source and installed-SDK smoke gaps.
4. Close `RDY-020` source/package manifest and package-smoke gaps, including
   the constrained Blender evidence connector.
5. Repair and enforce `DOC-410` so the resulting evidence is deterministic and
   current.

Every item will be re-audited against its own acceptance criteria before the
next dependent wave begins.
