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

## Credit

We are happy to credit security researchers in the changelog and release notes. Let us know in your report how you would like to be credited.
