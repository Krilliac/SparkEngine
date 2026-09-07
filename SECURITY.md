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
and enforced by `tools/check-supply-chain.py`, a required CI gate on every push
and pull request. The checker verifies:

- **Content integrity**: SHA-256 hashes and git blob identity for 39 sentinel
  files (entry-point headers, implementation files, license texts)
- **Submodule identity**: Exact gitlink SHAs for 6 git submodules, reconciled
  bidirectionally against `.gitmodules`
- **Directory inventory**: Every directory under `ThirdParty/` must be declared
  as a managed vendored directory, project-owned directory, or submodule
- **Action pinning**: All GitHub Actions workflow references must use full
  40-character commit SHAs
- **License coverage**: Every dependency must have a license file with copyright
  and operative terms
- **Manifest reconciliation**: License notices referenced in
  `ThirdParty/dependencies.lock` must be tracked as sentinel files
- **Fail-closed verification**: missing or malformed policy data, schema errors,
  Git failures, and path-escape attempts are errors; policy violations exit 1
  and verifier failures exit 2.

Verification: `python tools/check-supply-chain.py`
CI job: `check-supply-chain` in `.github/workflows/build.yml`

**Outstanding (SEC-110 remains open/blocking):**

- SBOM generation and release provenance
- Vulnerability-scanner integration
- Required secret-scanning enforcement
- CodeQL coverage for every shipped product
- A formal severity-exception schema with owner and expiry

## Credit

We are happy to credit security researchers in the changelog and release notes. Let us know in your report how you would like to be credited.
