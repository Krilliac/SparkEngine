# SparkEngine Third-Party Dependency Policy

## Scope

This policy governs all code and data under `ThirdParty/` — the sole location
for third-party dependencies in the SparkEngine repository.

## Authoritative Lockfiles

| File | Purpose |
|------|---------|
| `ThirdParty/supply-chain.lock` | Content hashes, git blob identity, submodule gitlinks, managed directory inventory |
| `ThirdParty/dependencies.lock` | Dependency metadata: name, source URL, version, license, required files, feature macro, fallback, severity, notice files |
| `.gitmodules` | Submodule URL and branch configuration |

All three must agree. The CI job `check-supply-chain` enforces consistency on
every push and pull request.

## Adding a New Dependency

1. **Justify the addition.** A new dependency must solve a problem that cannot
   be handled by existing code or a simpler approach.
2. **License review.** Only MIT, BSD-2-Clause, BSD-3-Clause, ISC, zlib, Boost,
   Apache-2.0, and public-domain licenses are pre-approved. Other licenses
   require explicit maintainer approval.
3. **Vendor or submodule.** Small single-header libraries are vendored directly.
   Larger projects use git submodules pinned to an exact commit SHA.
4. **Update the lockfiles:**
   - Add the entry to `ThirdParty/dependencies.lock` (all 10 fields).
   - Add license files to `ThirdParty/Licenses/` (for submodules) or alongside
     the vendored code.
   - Run `python tools/check-supply-chain.py --update` to regenerate
     `supply-chain.lock`.
5. **Wire it in.** The dependency must be used (or gated behind a feature macro)
   — dead dependencies are removed.

## Updating an Existing Dependency

1. **Submodules:** Update the gitlink (`git -C ThirdParty/X checkout <new-sha>`,
   then `git add ThirdParty/X`).
2. **Vendored code:** Replace the files, verify license compatibility.
3. **Regenerate lockfile:** `python tools/check-supply-chain.py --update`
4. **Update manifest:** If the version string changed in `dependencies.lock`,
   update it.

## Content Integrity

Every sentinel file (entry-point headers, implementation files, license texts)
is locked by SHA-256 content hash, git blob SHA (platform-stable), and size in
`supply-chain.lock`. The CI checker verifies these on every run:

- **Hash mismatch** → build fails (content was modified without lockfile update)
- **Git blob drift** → build fails (content changed relative to git object store)
- **Size mismatch** → build fails
- **Missing file** → build fails
- **Unmanaged directory** → build fails (new code appeared without policy review)

## Path Safety

All lockfile paths must be repository-relative, under `ThirdParty/`, with no
absolute paths, dot-segments (`..` or `.`), backslashes, or escape sequences.
Symlinks, junctions, hardlinks, and reparse points are rejected at verification
time. File resolution is checked against the repository root.

## License Coverage

Every dependency must have a license file that:
- Is at least 200 bytes
- Contains a copyright statement
- Contains operative license terms (grant clause)

The `cmake/SparkThirdPartyAudit.cmake` module additionally validates these at
CMake configure time.

## GitHub Actions Pinning

All workflow actions must be pinned to full 40-character commit SHAs. Tag-only
or branch-only references are rejected by the supply-chain checker.

## Enforcement

- **CI:** `check-supply-chain` job in `.github/workflows/build.yml`
- **Pre-merge:** Required CI gate includes supply-chain check
- **Tooling:** `python tools/check-supply-chain.py` (local), `--json` for
  machine-readable output, `--update` for regeneration
