# SparkEngine Third-Party Dependency Policy

## Scope

This policy governs all code and data under `ThirdParty/`. New third-party code
belongs there and nowhere else — but note that this is a review rule, not a
tooling guarantee: no check currently scans the rest of the repository for
vendored code, so a library dropped into `SparkEngine/Source/` would be outside
every check described below.

## Authoritative Lockfiles

| File | Purpose |
|------|---------|
| `ThirdParty/supply-chain.lock` | Container inventory, per-container tree digests, content hashes, git blob identity, submodule gitlinks, pinned action identities |
| `ThirdParty/dependencies.lock` | Dependency metadata: name, source URL, version, license, required files, feature macro, fallback, severity, notice files |
| `.gitmodules` | Submodule path and URL configuration |

The `check-supply-chain` CI job reconciles all three against each other and
against the git index:

- `supply-chain.lock` ↔ `.gitmodules` — bidirectional. A submodule present in
  one and absent from the other fails.
- `dependencies.lock` → `supply-chain.lock` — every dependency path must be an
  **exact** declared container (not a prefix of one), and every license notice
  it names must be a sentinel of type `license`.
- `supply-chain.lock` → `dependencies.lock` — every managed vendored directory
  and every submodule must have a manifest entry recording its provenance.
  Directories declared `project_owned` are exempt, and each one must carry a
  written justification in the lockfile explaining why it is first-party.
- `dependencies.lock` ↔ `.gitmodules` — a submodule's manifest source URL must
  equal its `.gitmodules` URL, and its manifest version must equal the locked
  gitlink revision.

The manifest is expanded through CMake before reconciliation, so entries added
by `list(APPEND ...)` after the closing parenthesis, entries split across lines,
and entries built from variables are all seen. A text scrape of the manifest
sees none of them and reports a clean tree it never examined.

## Adding a New Dependency

1. **Justify the addition.** A new dependency must solve a problem that cannot
   be handled by existing code or a simpler approach.
2. **License review.** Only MIT, BSD-2-Clause, BSD-3-Clause, ISC, zlib, Boost,
   Apache-2.0, and public-domain licenses are pre-approved. Other licenses
   require explicit maintainer approval. This allowlist is enforced by
   maintainer review; no tool validates the manifest's `license` field against
   it.
3. **Vendor or submodule.** Small single-header libraries are vendored directly.
   Larger projects use git submodules pinned to an exact commit SHA.
4. **Update the lockfiles:**
   - Add the entry to `ThirdParty/dependencies.lock` (all 10 fields).
   - Declare the directory in `supply-chain.lock` under `managed_vendored_dirs`
     (or `project_owned_dirs`, with a justification, for first-party code).
   - Add license files to `ThirdParty/Licenses/` (for submodules) or alongside
     the vendored code.
   - Run `python tools/check-supply-chain.py --update` to regenerate the derived
     fields.
5. **Wire it in.** The dependency must be used (or gated behind a feature macro)
   — dead dependencies are removed.

## Updating an Existing Dependency

1. **Submodules:** Update the gitlink (`git -C ThirdParty/X checkout <new-sha>`,
   then `git add ThirdParty/X`).
2. **Vendored code:** Replace the files, verify license compatibility.
3. **Regenerate lockfile:** `python tools/check-supply-chain.py --update`
4. **Update manifest:** If the version string changed in `dependencies.lock`,
   update it.

`--update` recomputes hashes and digests from the working tree and then re-runs
the full verification over what it wrote, so it cannot report success on a
lockfile that does not verify. It still cannot tell an intended change from an
unintended one: **review the lockfile diff before committing, and never run it
to make a failing check pass.** It refuses to run at all if the lockfile is
missing, because the container declarations carry policy decisions it cannot
invent.

## Content Integrity

Coverage is complete, not sampled. Every tracked path under `ThirdParty/` is
enumerated from the git index and must belong to exactly one declared container.
Each non-submodule container carries a SHA-256 digest over the full
`(mode, blob, path)` set of its tracked files; each submodule is pinned by its
gitlink; the only files exempt are the governance files named in
`allowed_root_files`.

Sentinel files — entry-point headers, implementation files, and license texts —
add on-disk verification on top of that: SHA-256 content hash, git blob
identity, and size. The content hash and size use a streaming LF-normalized
view, so Windows CRLF checkout bytes and POSIX LF checkout bytes carry the same
evidence without weakening the raw-size ceiling.

The CI checker verifies these on every run, and any failure fails the
`check-supply-chain` job:

- **Tree digest drift** → CI fails (tracked payload was added, edited, renamed,
  or removed at any depth)
- **Tracked file in no declared container** → CI fails (new code appeared
  without policy review)
- **Working tree disagrees with the index** → CI fails (untracked, modified, or
  deleted payload is unverifiable)
- **Hash, blob, or size mismatch on a sentinel** → CI fails
- **Missing sentinel file** → CI fails

Nothing is compiled by this checker; it is a policy gate, not a build.

## Path Safety

All lockfile paths must be repository-relative, under `ThirdParty/`, with no
absolute paths, dot-segments (`..` or `.`), backslashes, trailing slashes, or
unsafe characters.

The `ThirdParty/` tree is walked with `lstat`, which never follows, and any
symbolic link or reparse point is rejected at any depth — including a Windows
directory junction, which reports `is_symlink() == False` and whose reparse bit
is cleared by a following `stat()`, so a stat-based check can never see it.
Sentinel files are additionally rejected if they are hardlinked or if they
resolve outside the repository root. The walk is bounded in depth and entry
count and never descends through a rejected entry.

Container declarations must be pairwise disjoint across
`managed_vendored_dirs`, `project_owned_dirs`, and `submodule_gitlinks`; no
container may be nested inside another; and case-variant aliases are rejected,
because such an alias verifies only on a case-insensitive filesystem.

## License Coverage

Every dependency in `dependencies.lock` must name at least one license notice
file. Each notice, and every `type: "license"` sentinel, must:

- Be at least 200 bytes
- Contain a copyright statement
- Contain operative license terms (grant clause)

`cmake/SparkThirdPartyAudit.cmake` enforces these on the manifest's notice files
at CMake configure time as a fatal error, independent of `SPARK_STRICT_DEPS`.
The Python checker enforces the same rules on every license sentinel.

## GitHub Actions Pinning

Workflows under `.github/workflows/` (at any depth) and every composite-action
`action.yml`/`action.yaml` in the repository are parsed with a YAML parser. Each
`uses:` value is checked after the parser has resolved quoting, flow mappings,
block scalars, anchors, aliases, merge keys, and line continuations, so none of
those forms can carry an unpinned reference past the check.

- Registry actions must be `owner/repo[/path]@<40 lowercase hex>`, and both the
  `owner/repo` identity and that SHA must be recorded in the lockfile's
  `action_pins`. A pin answers "did it change"; the recorded identity answers
  "whose is it", and a repointed pin shows up as a lockfile diff for review.
- Docker actions must be `docker://<image>@sha256:<64 lowercase hex>`.
- Local actions (`./path`) must resolve, inside the repository, to a directory
  containing an `action.yml` or `action.yaml`, which is then checked in turn.
- A `uses:` value containing a `${{ ... }}` expression is rejected: it cannot be
  pinned.

A missing `.github/workflows` directory is an error, not a warning — a check
that did not run is not a check that passed.

## Enforcement

- **CI:** `check-supply-chain` job in `.github/workflows/build.yml`, plus the
  adversarial suite `Tests/test_check_supply_chain.py` in the same job and
  `Tests/Tools/test_check_thirdparty_manifest_sync.py` in `validate-ci-tools`.
  Both jobs are members of the required-CI gate.
- **Tooling:** `python tools/check-supply-chain.py` (local), `--json` for
  machine-readable output, `--update` for regeneration. It requires PyYAML and
  CMake and exits 2 without them rather than degrading to a weaker check.
- **Manifest sync:** `tools/check-thirdparty-manifest-sync.sh` requires a
  `dependencies.lock` bump whenever ThirdParty payload or dependency wiring
  changes. The governance files at the ThirdParty root (`POLICY.md`,
  `README.md`, `supply-chain.lock`) are exempt from that trigger, because they
  carry no payload and are reconciled by the checker above.
