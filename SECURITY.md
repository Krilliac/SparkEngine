# Security Policy

## Supported Versions

SparkEngine has not published a certified versioned release. The declared
`stable-v1` profile is blocked and uncertified, so there is no released version
line with a security-support commitment.

| Release line | Status |
|---|---|
| `stable-v1` | Pre-release and blocked; no supported version has been published |
| `Working` / nightly artifacts | Development evaluation only; fixes are best-effort and do not constitute a release SLA |

## Reporting a Vulnerability

If you discover a security vulnerability in SparkEngine, please report it responsibly using **GitHub Security Advisories**:

1. Go to the [Security Advisories page](https://github.com/Krilliac/SparkEngine/security/advisories/new)
2. Click **"New draft security advisory"**
3. Fill in the details of the vulnerability

This ensures your report is private and only visible to the maintainers until a fix is available.

**Please do NOT open a public issue for security vulnerabilities.**

## Response Timeline

- **Acknowledgment**: Within 48 hours of report submission
- **Triage**: Within 7 days — we will confirm whether the issue is valid and assess severity
- **Fix**: Within 30 days for critical issues, 90 days for lower severity

## Scope

SparkEngine's first-party gameplay, discovery, collaboration, and live-editor endpoints default to loopback. A development LAN endpoint must name one canonical RFC1918 interface and prefix (for example `192.168.1.20/24`); the entire subnet must remain inside RFC1918 space, and its exact network and directed-broadcast addresses are rejected. Peers are limited to that same subnet. Wildcard, public, documentation, multicast, limited-broadcast, CGNAT, IPv4-mapped IPv6, and alternate textual forms are rejected before socket creation. This is a containment boundary, not transport security: the active UDP protocol is unauthenticated and unencrypted, and the legacy XOR/FNV helpers must not protect credentials or hostile-network traffic. NET-100 remains open for the experimental multiplayer surface, which is outside the single-player, service-free `stable-v1` profile and does not certify it.

The SEC-120 structural fuzz-policy gate inventories stable-v1 file/package parser
surfaces across every first-party source tree and blocks unclassified additions, unsafe
manifest paths, malformed schemas, unreviewed scope exclusions, and incomplete
corpus/resource bindings. A parser may only be marked as fuzzed when a tokenized read of
the CMake and C++ proves a declared, build-reachable, sanitizer-instrumented target, a
registered CTest entry point that passes the corpus directory and exact runtime limits,
and a harness that calls the production entry point — a commented-out or string-literal
declaration proves nothing. It is not runtime parser-safety or fuzz-coverage evidence.
SEC-120 remains release-blocking, and `release.yml` enforces that with
`check_fuzz_policy.py --require-closure`, because no production parser fuzz target,
bounded seed corpus, sanitizer smoke campaign, or scheduled campaign is currently
committed: 108 inventoried parsers are blocked and 149 detected candidates are still
deferred. See `wiki/advanced/Fuzz-Policy-and-Parser-Security.md` for the exact scope,
the reviewed exclusions, and the closure blockers.

The following are considered security vulnerabilities:

- Memory safety bugs (buffer overflows, use-after-free, out-of-bounds access)
- Remote code execution via asset loading (malicious models, textures, scripts, scenes)
- Network protocol exploits (packet injection, denial of service, authentication bypass)
- Path traversal in asset or file loading
- Arbitrary code execution through the scripting engine sandbox

The following are **not** in scope:

- Game logic exploits or cheating in multiplayer (these are game-specific, not engine bugs)
- Denial of service via excessive resource usage in the editor
- Issues requiring physical access to the machine
- Vulnerabilities in third-party dependencies (report these to the upstream project)

## Supply-Chain Security

Third-party dependencies are governed by [`ThirdParty/POLICY.md`](ThirdParty/POLICY.md)
and enforced by `tools/check-supply-chain.py` in the required
`check-supply-chain` CI job. The checker verifies:

- **Complete tracked payload coverage:** every tracked `ThirdParty/` path is
  assigned to exactly one declared container. Each non-submodule container has a
  SHA-256 tree digest over its complete `(mode, blob, path)` set, so tracked
  additions, edits, deletions, and renames at any depth are detected.
- **Content and submodule identity:** declared sentinel files have SHA-256, Git
  blob, and size checks; gitlink SHAs reconcile bidirectionally with
  `.gitmodules`.
- **Link hygiene:** symbolic links, Windows reparse points, hardlinked
  sentinels, and containment escapes are rejected.
- **Structured action pinning:** workflow and composite-action YAML is parsed,
  not line-scanned. External actions must be lockfile-authorized full-SHA pins,
  Docker actions must be digest-pinned, and local actions must resolve to
  `action.yml`.
- **Manifest reconciliation:** CMake expands `ThirdParty/dependencies.lock`,
  which is then reconciled bidirectionally with the supply-chain lock across
  identity, license, feature, fallback, and notice metadata.
- **Resource bounds and fail-closed behavior:** policy inputs and traversals are
  bounded; malformed, unsafe, or unverifiable data fails rather than falling
  back to a weaker check.

Verification: `python tools/check-supply-chain.py`
CI job: `check-supply-chain` in `.github/workflows/build.yml`

**Outstanding (SEC-110 remains open/blocking):**

- SBOM generation and release provenance
- Vulnerability-scanner integration
- Required secret-scanning enforcement
- CodeQL coverage for every shipped product
- A formal severity-exception schema with owner and expiry
- SPDX allowlist enforcement and policy for third-party code outside
  `ThirdParty/`

## Credit

We are happy to credit security researchers in the changelog and release notes. Let us know in your report how you would like to be credited.
