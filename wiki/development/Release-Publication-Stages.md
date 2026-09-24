# Release Publication Stages

**Audience:** release maintainers and qualification owners.

**Thread context:** stable-v1 publication and the repository readiness contract.

**Platform/backend scope:** Windows 11 x64, MSVC v143, D3D11 and NullRHI.

## Candidate qualification

Publication uses `python3 tools/site-data/validate.py --require-candidate-ready`.
The final `--require-ready` check still requires all work and gates complete;
it is not the entry condition for publishing the evidence needed to finish.

The profile's `publicationFinalization` object declares the terminal work IDs,
their required terminal gate, and the `stable-release` environment. Those IDs
must exactly match applicable work explicitly typed with
`completionPhase: publication-finalization`. This is a closed metadata schema,
not an arbitrary exemption list. Technical work cannot depend on a finalizer.

Before publication, all other blocking items and transitive dependencies must
be done, with no planned verification left. All required technical gates must
pass. Gates that also await a finalizer must be `at-risk`, with their other work
complete; they cannot be described as passing. Finalizers must stay `in-progress`.
REL-190 owns supported-host rehearsal, migration/rollback/recovery drills,
qualification sign-off, the `profile-required-gates` and `release-approval` jobs,
and their planned rehearsal selectors. It must be done before candidate
publication. REL-200 depends on it and owns only publish/download/site
finalization, with no deferred technical job or rehearsal selector. A finalizer
carrying planned verification is rejected.
Every profile and global state stays `candidate`, with an assigned owner and
reviewed qualification sign-off. Nothing in this procedure promotes the current
blocked ledger or substitutes fixture tests for real qualification evidence.

## Contract reference rules

`validate.py` also enforces these rules on every run. A capability can be
`ready` only if it names at least one required gate and has evidence. A gate can
be `passing` only if it has evidence. Every work-item ID (such as `RDY-000`) and
gate ID (such as `G00`) named in contract text must be declared, including
rationale, summaries, limitations and website copy. `FUTURE_ACCEPTANCE_PATHS` in
`validate.py` may list only paths that are still missing on disk and still named
by an unfinished work item's `entryPoints`/`documentationUpdates` or by the docs
catalog; a reference from a `done` item does not count. When a path lands,
delete its entry in the same change. A work item marked `done` never resolves a
reference through that list.

Work-item `commands` are resolved against `CMakePresets.json`, using the
inheritance resolver in `Tools/buildmatrix/inventory.py`. `cmake --preset X`
must name a configure preset, `cmake --build --preset X` a build preset, and
`ctest --preset X` a test preset. Every `build/<dir>` tree passed to cmake or
ctest must be a configure preset's binaryDir. A ctest run must target a preset
that builds tests. A preset whose resolved `BUILD_TESTS` is false, such as
`windows-shipping` or `linux-shipping`, is accepted only when the same item
configures that preset with `-DBUILD_TESTS=ON` (as RDY-010 and PLT-200 do).
Otherwise the run belongs on a validation preset such as `windows-release`
(`-C Release`), `linux-gcc-release`, or `ci-linux-asan`. A preset an item will
add is declared in `PLANNED_CMAKE_PRESETS` in `validate.py` and keyed to its
owning item. Only that owner's cmake commands may name it, and never a ctest
tree. The entry is an error once the preset exists, the owner is `done`, or the
owner stops naming it.

Hand-written counts are governed too. `validate.py` scans every file in
`REQUIRED_GLOBAL_PUBLIC_CLAIM_SURFACES` for a number followed (within two words)
by tests, files, panels, modules, subsystems, backends, lines or nodes, including
`N+`, `~N` and `N/M` forms and phrases wrapped across lines. `<!-- AUTO:* -->`
blocks, the fully generated `wiki/advanced/Codebase-Statistics.md`, and the
`sed_replace` patterns of `docs/update-readme-badges.sh` are generator-owned and
skipped. Each pattern is skipped only on the file(s) its own `sed_replace` call
rewrites (the validator resolves `$readme`, loop variables, and arrays; an
unresolvable target fails validation), so `N specialized panels` is managed on
`README.md` but must be claimed on `wiki/getting-started/FAQ.md`. Every other hit
must lie wholly inside the `text` of a `readiness.publicNumericClaims` entry for
that `surface`; an entry `64 nodes` does not cover `~64 nodes`, `1/64 nodes` or
`#64 nodes`:

| `classification` | Extra field | Check |
|------------------|-------------|-------|
| `metric` | `metricId` | Exactly one claim, compared with the value `generate.py` measures from source; `N+` passes while the metric is at least `N`, and `~N` or `N/M` is refused |
| `static-fact` | `evidencePath` | The cited path must exist (design constants such as the 4096-node mod JSON budget) |
| `historical` | none | A dated audit or changelog record that is not re-measured |

An entry whose text no longer occurs, or holds no claim, is an error, so
rewording a page retires its entry in the same change. Prefer removing
per-file line counts over registering them: they drift with every edit.

CTest runs the whole contract suite as `site-data-contract`. The faster
`readiness-cross-references` runs the strict live validation plus the dependency,
promotion, selector, future-path, prose-reference and handoff cases. Both carry
the `readiness` and `site-data` labels and are registered on non-Windows hosts
only; the Linux `site-data` workflow is the suite's CI home.

## Protected publication authority

The repository owner must create the `stable-release` GitHub environment before
dispatching a versioned build. This repository has one authorized human release
provider, `Krilliac`, so the environment uses an explicit owner-only contract:
configure exactly one required reviewer, the repository owner, and allow that
owner to approve a release they initiated. The required environment approval is
still mandatory; this is not an approval bypass and no second reviewer is
invented. The read-only verifier proves the reviewer identity through the
repository API. Use a custom deployment policy with exactly one branch entry
named `Working`. Do not allow wildcard or tag entries.
The controller runs from the current trusted `Working` commit, even when its
publication target is a version tag. Keep the existing Working ruleset enforced.
Disable administrative protection bypass in the environment's settings. The API
must prove `can_admins_bypass=false`; true, missing, or unrecognized values fail.

The workflow checks these API-visible protections before building, on entry to
the publication job, and immediately before publishing. The publisher job itself
is bound to `stable-release`, so GitHub applies the required owner approval gate.
Administrative bypass remains disabled. Nightly uses
the separate `nightly-release` environment and cannot qualify stable-v1.

`verify_release_environment.py` only reads GitHub metadata. Its token needs
repository Actions read access for the environment and deployment-branch APIs.
The owner must resolve a missing environment, API denial, or unavailable
protection feature; none is treated as approval. The preflight never creates an
environment, broadens token permissions, or supplies a substitute approval.

Immediately after that entry check, the publisher records the actual approval
event with `record_release_approval.py`. It reads
`GET /repos/{repo}/actions/runs/{run_id}/approvals` (bounded, Link-paginated,
duplicate-key-rejecting) and binds it to the run attempt, head SHA, release
workflow path, repository, and `stable-release` environment id. It fails unless
the history holds at least one approval, every review is `approved` by the
repository owner `Krilliac` (login and id) for `stable-release` only, and no
review is rejected. Each environment-bound job needs its own review, so a stable
run normally carries more than one owner approval; all of them are recorded.
The closed, deterministic record keeps the run id, attempt and start time, the
environment id, the approver, and a SHA-256 of each review comment. It is
retained as the 90-day artifact
`stable-release-approval-<SHA>-<run>-<attempt>` and its digest becomes the
job output `approval_record_sha256`. The approvals API exposes no review
timestamp, so approval time is bounded by the run start and that artifact's
upload. An owner-only approval is one person's review, not independent
second-person review; how GitHub reports that self-approval still needs a
hosted stable run to confirm.

The separate repository `/immutable-releases` API requires **Administration
read** permission, which `GITHUB_TOKEN` cannot provide. The owner must provision
`RELEASE_POLICY_READ_TOKEN` in both the `stable-release` and `nightly-release`
environments, using a GitHub App installation token or fine-grained token scoped
to this repository with Administration read (plus implicit Metadata read).
It needs no contents write or other write permission. Keep token issuance and
renewal outside this workflow's code changes; do not place it in source or a
repository variable. Policy proof runs after the environment-bound job starts,
not in the unprivileged preparation job. Missing, expired, or denied policy
authority fails closed and requires owner setup. Bindings are restricted to the
individual policy, guarded-mutation, staging, acceptance, and recovery steps;
there is no workflow/job-level binding. Checkout, download, upload, other
third-party actions, and unrelated repository steps do not receive this secret.

The policy reader supplies that secret only to its read subprocess. Mutation
commands keep the separate `GH_TOKEN` and do not receive the policy secret. The
policy credential is never passed on a command line, printed, or retained in the
evidence receipt; guard errors do not render mutation argv or Git auth headers.
The independent publication consumer receives neither environment secret.

Provision native Authenticode signing and the publisher thumbprint separately.
After the exact stable assets are frozen, the protected `stable-release` job
imports the same PFX with an ephemeral key and creates deterministic detached
signatures without exporting the private key. The derived SPKI SHA-256
fingerprint must match the protected `SPARKENGINE_STABLE_SIGNATURE_KEY_FINGERPRINT`
repository variable. The job uploads the resulting flat
`SparkEngine-release-signature-bundle.tar.gz` as an immutable release control
asset only after the durable download-counter preflight; the control asset is
not a distributable or badge-ledger entry. All existing signature, checksum,
SBOM, scan, exact-CI, source/tag, and package qualification gates still apply.

## Immutable stable and rolling-nightly policy conflict

Stable publication requires repository immutable releases enabled. All package,
signature, checksum, SBOM, exact-CI and source/tag checks complete against the
fully populated draft before the irreversible publication boundary. After
publication the returned record must be immutable, and `gh release verify`
verifies its automatic immutable-release attestation. A post-publication failure
preserves an immutable or ambiguous target for owner investigation. If an exact
fresh response instead proves that the same stable release ID, tag, and channel
are public and mutable, the acceptance/recovery helper may quarantine that target
as a draft and verify the result. It never infers permission to hide an immutable,
misidentified, or ambiguous release. Either failure leaves final readiness blocked.

The current rolling-nightly design needs mutable releases, so these two channel
policies are mutually exclusive today. The policy preflight runs inside the
environment-bound publisher, and each ordinary publication/staging Git push or
API write checks again in its own call path. The narrowly proven mutable-stable
quarantine above is a containment exception, not permission to publish under an
incompatible policy. Draft staging checks before each create, update, asset deletion,
and upload, instead of delegating multiple unchecked mutations to a release
action. Policy-flip tests prove that later writes are refused after a compatible
policy changes. Under immutable policy, nightly fails without hiding, replacing,
or publishing a release. Its recovery writes also recheck policy. Nightly retains
mutable redraft recovery and does not run immutable-release verification.
The workflow never toggles the repository policy automatically.

This remains a release blocker in REL-100 and REL-190. Both channels must not be
described as operational together. A separately reviewed channel migration to
unique immutable nightly tags or Actions artifacts, or an explicit owner-approved
channel policy change, is required. That policy work is outside this change.

## Independent verification and final readiness

After publication, `verify-stable-publication` runs on a fresh runner with only
Actions, contents, and attestations read permissions. It obtains the exact
published asset IDs through GitHub, downloads the stable assets and the
signature control asset, checks their sizes and SHA-256 digests, extracts and
verifies the pinned detached signatures and SBOM, compares provenance to freshly
revalidated exact-CI evidence, verifies the GitHub release attestation, and
rechecks the release, assets, and tag for drift. Before downloading anything it
rebuilds the approval record from the GitHub API for the publisher's attempt and
requires the publisher's exact `approval_record_sha256`; the receipt embeds it.
It cannot publish, edit the ledger, or turn the profile ready.

Only success produces the immutable Actions artifact
`stable-publication-evidence-<SHA>-<run>-<attempt>`, retained for 90 days. The
receipt says `publication-verified`, carries the candidate SHA, release and asset
IDs/digests, and verifier run identity. Preserve that artifact and its GitHub
artifact digest/producer identity in the release evidence archive before expiry;
an unverified local JSON copy is not authority. A failed verifier run produces no
success artifact, even if a late local publication error leaves a visible JSON
file. Final readiness remains blocked pending investigation and recovery.

The receipt writer accepts at most 1 MiB and requires an existing real parent
directory. It rejects existing names and symlink/reparse traversal. Linux
publication anchors an anonymous `O_TMPFILE` inode to a directory descriptor,
syncs the completed file, atomically links the owned fd without replacement, and
syncs the directory before verifying the visible file and parent identities.
The [Linux-documented proc fd link mechanism](https://man7.org/linux/man-pages/man2/open.2.html)
needs no elevated capability. Missing `O_TMPFILE`, proc fd access, or directory
sync support fails closed; other POSIX platforms have no named-temp fallback.
Pre-link failure closes the anonymous inode, leaving no staging name to clean
up. Post-link failure never deletes a visible name: it reports that publication
or durability is unconfirmed, preserves any competing replacement, and requires
investigation rather than issuing a successful receipt result. Windows retains
the unchanged native package writer under locked parent directories. Neither
backend overwrites an existing receipt.

The release owner must then reconcile this independent evidence, actual
deployment approval, all supported-host/rehearsal results, and live-site
consumption at the same released SHA. Only that completed review may close the
declared finalization work and promote all required gates/profile/global state.
`--require-ready` remains the final fail-closed validator. A successful publisher
or consumer alone is insufficient. The released candidate commit is immutable;
later evidence-recording commits must retain that exact artifact source identity.

## First-release predecessor policy

The maintainer chose a signed, immutable `v0.9.0` stable predecessor before
`v1.0.0`, rather than a first-release bootstrap exception. The strict MSI
qualifier must continue to require a real previously published, signed,
non-prerelease stable MSI and its matching manifest. Publish `v0.9.0` only after
its own applicable signing, package, publication, and acceptance evidence is
complete; then qualify the `v1.0.0` upgrade, rollback, repair, and user-data
retention gates against those exact immutable predecessor assets. The choice
does not mark either release ready or waive any gate. Until `v0.9.0` is
independently verified as published, the provisioner correctly rejects
`v1.0.0`. Fabricating an older release, treating nightly as the predecessor,
or marking upgrade/rollback done without the real transaction is prohibited.

The maintainer chose to derive a reviewed `v0.9.0` candidate from the current
release branch in an isolated checkout after its source batch is sealed. That
does not authorize relabeling the current `1.0.0` checkout in place or
manufacturing an old tag: the candidate needs its own reviewed version and
changelog commit, exact-source CI, signed artifacts, and publication evidence.
The release controller requires the stable tag to match the single CMake
version default and its changelog heading. No `v0.9.0` source/tag or published
release currently satisfies that rule. The existing
versioned MSI job also always requires a prior release; it is intentionally not
a `v0.9.0` bootstrap path. Before any `v0.9.0` dispatch, the owner must review
and identify an authentic `0.9.0` source commit with corresponding version and
changelog, and approve a separate one-time native qualification contract for a
signed clean install, repair, runtime smoke, uninstall, and user-data retention
without pretending to perform an upgrade or rollback. That contract must still
pass the applicable exact-source CI, package, signing, protected-environment,
immutable-publication, and independent-consumer gates. Only after the resulting
`v0.9.0` release is independently verified should the normal `v1.0.0` gate run
its actual old-to-new upgrade/rollback transaction. The normal provisioner pins
`v1.0.0` specifically to an immutable `v0.9.0` release; a different earlier
version or a mutable release cannot substitute for it.

## Source & Freshness

Implemented 2026-09-21. Sources: [readiness contract](../../docs/site/readiness.json),
[release workflow](../../.github/workflows/release.yml), and
[GitHub environment API](https://docs.github.com/en/rest/deployments/environments),
and the [repository immutability API](https://docs.github.com/en/enterprise-cloud%40latest/rest/repos/repos#check-if-immutable-releases-are-enabled-for-a-repository).
Predecessor identity and source-lineage review updated 2026-09-22; see the
[release API immutable field](https://docs.github.com/en/rest/releases/releases).
Approval-event recording added 2026-09-24 against the
[workflow-run review history API](https://docs.github.com/en/rest/actions/workflow-runs#get-the-review-history-for-a-workflow-run).
Recheck live environment protection, approval history, signing authority, and
exact-SHA run/artifact identities before each release.
Contract reference rules and the public numeric-claim ledger added 2026-09-24 from
[`validate.py`](../../tools/site-data/validate.py) and
[`test_site_data_contract.py`](../../Tests/Tools/test_site_data_contract.py).
