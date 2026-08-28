# Security Policy

## Supported Versions

| Version | Supported          |
|---------|--------------------|
| 1.0.x   | Yes                |
| < 1.0   | No                 |

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

SparkEngine enforces a dependency supply-chain policy covering all third-party
code under `ThirdParty/`. Key controls:

- **Pinned identity:** Every submodule is locked to an exact commit SHA in
  `ThirdParty/supply-chain.lock`. Vendored snapshots are locked by SHA-256
  content hash. Any drift is a CI failure.
- **License coverage:** Every dependency must ship a license file containing a
  copyright statement and operative terms. The CI checker and CMake audit both
  enforce this.
- **Action pinning:** All GitHub Actions are pinned to full 40-character commit
  SHAs — tag-only or branch-only references are rejected.
- **Inventory completeness:** No unmanaged code may appear under `ThirdParty/`
  without being declared in the lockfile.
- **Fail-closed:** Missing lockfile, corrupt JSON, unrecognized version, or any
  verification error causes the check to exit non-zero.

Policy details: [`ThirdParty/POLICY.md`](ThirdParty/POLICY.md)
Verification: `python tools/check-supply-chain.py`
CI job: `check-supply-chain` in `.github/workflows/build.yml`

## Credit

We are happy to credit security researchers in the changelog and release notes. Let us know in your report how you would like to be credited.
