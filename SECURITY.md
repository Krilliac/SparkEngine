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

## Response Expectations

SparkEngine does not currently promise an acknowledgment, triage, or fix
timeline. Any response is best-effort and depends on maintainer availability,
severity, reproducibility, and whether a supported release exists. These
expectations will be revisited as part of the release-governance review.

## Scope

SparkEngine's first-party gameplay, discovery, collaboration, and live-editor endpoints default to loopback. A development LAN endpoint must name one canonical RFC1918 interface and prefix (for example `192.168.1.20/24`); the entire subnet must remain inside RFC1918 space, and its exact network and directed-broadcast addresses are rejected. Peers are limited to that same subnet. Wildcard, public, documentation, multicast, limited-broadcast, CGNAT, IPv4-mapped IPv6, and alternate textual forms are rejected before socket creation. This is a containment boundary, not transport security: the active UDP protocol is unauthenticated and unencrypted, so it must not carry credentials or face hostile-network traffic (the legacy XOR prototype has been removed, and the unreviewed in-tree `SecureChannel` is not wired into it). NET-100 remains open for the experimental multiplayer surface, which is outside the single-player, service-free `stable-v1` profile and does not certify it.

The SEC-120 structural fuzz-policy gate inventories stable-v1 file/package parser
surfaces across every first-party source tree and blocks unclassified additions, unsafe
manifest paths, malformed schemas, unreviewed scope exclusions, and incomplete
corpus/resource bindings. A parser may only be marked as fuzzed when a tokenized read of
the CMake and C++ proves a declared, build-reachable, sanitizer-instrumented target, a
registered CTest entry point that passes the corpus directory and exact runtime limits,
and a harness that calls the production entry point — a commented-out or string-literal
declaration proves nothing. It is not runtime parser-safety or fuzz-coverage evidence.
SEC-120 remains release-blocking, and `release.yml` enforces that with
`check_fuzz_policy.py --require-closure`. Three production parsers (`json-utils`,
`neural-weights-nnw`, and `crash-manifest-parser`) now have structurally bound
sanitizer targets and bounded seed corpora. The current inventory still records 102
blocked parser targets and 151 deferred candidates; exact-SHA hosted smoke,
runtime coverage, and scheduled campaign evidence are not yet retained. See
`wiki/advanced/Fuzz-Policy-and-Parser-Security.md` for the exact scope, reviewed
exclusions, and closure blockers.

Data at rest (owner decision OD-22, work item DATA-120): first-party persistence stores passwords only as salted
PBKDF2 hashes, never persists session tokens, and reads no database credential from committed or shipped config;
`Tests/TestDATA120SecretsAtRest.cpp` exercises those paths against real files. SparkEngine does not encrypt its
database or save files. Operators who run a server are responsible for placing its save directory (and backups of it)
on host full-disk-encrypted storage; see `wiki/gameplay-tools/Persistence-System.md`. A plaintext password, session
token, or credential found in any first-party persisted file or shipped config is in scope as a vulnerability.

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

## Remote Administration

**Remote administration is unavailable in stable-v1, permanently.** Owner
decision OD-05 (`docs/readiness/OWNER-DECISIONS.md`) records that no
authenticated remote-administration channel is built for stable-v1. SparkEngine
has no remote RCON, no RemoteDebug network listener or remote connect entry
point, and no configuration value or command-line switch that enables either.
Administration is trusted local only.

SEC-100 tracks this and remains open. The current trust boundaries and audit
records are listed here so they can be reviewed; this section is not a review
sign-off, and the security owner and reviewer for the threat model are
**unassigned (required for SEC-100 closure)**.

Trust boundaries:

- **Chat is not administration.** Network chat is relayed and logged as one
  escaped field; it never reaches `DedicatedServer::ExecuteRcon`.
- **`ExecuteRcon` is in-process only.** No network transport calls it, and
  `ServerConfig` has no RCON password or port field that could enable one. The
  stable-v1 Windows Shipping product builds with `ENABLE_NETWORKING=OFF`, which
  compiles `DedicatedServer` out entirely.
- **RemoteDebug loopback is in-process only.** There is no `StartServer(port)`,
  `ConnectToTarget` or remote client `Connect`; `StartListening()` takes no port
  and binds no socket. Loopback mints an Observer principal whose lifetime can
  only be shortened (at most 5 minutes). Anonymous (including every raw queue
  call), invalid, expired, malformed, replayed, rate-limited and
  under-privileged requests are denied and audited.
- **Gateway area control is same-user local IPC, not remote administration.**
  It carries SparkServer/SparkGateway handoff phases only, and neither process
  ships in stable-v1 (`ENABLE_SERVER_PROCESSES=OFF`). Frames are accepted only from
  the same operating-system user and must carry an HMAC-SHA256 tag (verified in
  constant time), a timestamp within 60 seconds, and a nonce not already in the
  bounded replay ledger. A full ledger fails closed.

`Tests/TestSEC100RemoteAdminUnavailableReal.cpp` holds compile-time checks
that fail the build if any of these named entry points or fields returns:
`RemoteDebugSystem::StartServer`/`ConnectToTarget`,
`RemoteDebugClient::Connect`, a port-taking `RemoteDebugServer::StartListening`,
an endpoint on `RemoteSession`, and the `ServerConfig` fields `rconPassword`,
`rconPort`, `enableRcon` and `enableRemoteAdministration`. The field check is by
name and does not detect a differently named switch. Its runtime selectors
(`RemoteAdmin_Unavailable*`) prove a raw principal-less queue call is denied and
audited and the single local grant cannot reach an administrative command.
`Tests/Fixtures/NetworkingDisabledCompileContract.cpp` fails to compile in a
networking-off configuration if `DedicatedServer` or `ServerConfig` is declared
there. It is a local compile contract only: no hosted CI lane configures a
networking-off tree with tests enabled, so nothing enforces it on pull
requests yet. Closing SEC-100
still needs the reviewed threat model with a named security owner and reviewer,
and exact-SHA hosted `security-runtime`/`network-integration` evidence.

Audit records are written one per attempt through `Spark::Logger`.
`SparkServer` initializes that logger with a stderr sink when it starts (stdout
carries the health JSON), so the records reach whatever captures the server's
stderr; the per-reason counters are not exported outside the process. No record
carries a key, password, MAC, nonce, RCON argument, response body or exception
text. Chat text is
player-visible content and is logged escaped in one quoted field.

```text
RCON: command=<name|<redacted>|<unknown>> disposition=<dispatched|failed|unknown_command>
Chat: client=<id> bytes=<n> text="<escaped>"
GatewayAreaControl: reason=<reason> phase=<n> epoch=<n> session=<prefix> outcome=<applied|duplicate|rejected|unavailable>
```

Gateway `reason` is one of `accepted`, `incomplete`, `peer_mismatch`,
`oversize`, `wrong_service`, `decode_failed`, `phase_mismatch`,
`timestamp_window`, `mac_invalid`, `replay` or `ledger_full`. Denials are logged
at warning level, accepted handoff phases at info level, and readiness probes at
debug level. Before the MAC verifies, `phase`, `epoch` and `session` are
unauthenticated claims. `session` is cut to its first 16 characters, and any
character outside `[A-Za-z0-9._-]` becomes `?`. RemoteDebug keeps its newest 256
audit events in memory and counts every evicted event
(`GetDroppedAuditEventCount`).

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

**Secret scanning:** `tools/check-secret-scan.py` scans every git-tracked
regular file with the OPS-100 detectors in `tools/ops/secret_policy.py` (token
formats, PEM private-key headers, credential-bearing URLs, and the structured
credential lexer) in the required `secret-scan` CI job. The gate narrows two of
those detectors, and only these ways:

- *Structured credentials* (a credential-named key, then `=` or `:`, then a
  value): the last dotted segment of the key must name credential material, a
  `::` scope operator is not a separator, and values that are references (`$VAR`, `${...}`, `${{ ... }}`,
  `%s`, `{...}`, `<...>`) are not findings. In programming-language source
  (C/C++/HLSL/GLSL/Metal/Objective-C, Python, AngelScript, C#, JavaScript,
  TypeScript, Java, Kotlin, Go, Rust, Swift, Lua, Gradle) only a quoted string
  literal counts, because an unquoted right-hand side there is an expression.
  Every other file (configs, shell/batch/PowerShell scripts, Dockerfiles,
  dotfile and suffixless credential stores such as `.npmrc`, `.netrc` or an AWS
  `credentials` file, Markdown and text) also flags unquoted values, except a
  `$name`/`@name` key (a variable being read) and a YAML `key = ...` line.
- *Credential-bearing URLs*: a password part that is a reference is not a finding.
- *Binary files* (any NUL byte) get only the self-identifying detectors: PEM
  private-key headers and GitHub, OpenAI, Anthropic and AWS access-key token
  formats.

Symbolic links are never followed, files over the size bound fail instead of
being skipped, and findings report only `path:line: rule`, never the value. A
finding passes only through an exception in `tools/secret-scan-exceptions.json`
whose scope is exactly `<rule>:<tracked file path>` (no globs or directories),
using the same owner/justification/expiry schema as `supply-chain.lock`
exceptions plus a required `count` of reviewed findings, and expiring at most
366 days ahead. An exception covers its file only while the live count equals
the reviewed count: a new secret in an excepted file fails the job, and a lower
count must be ratcheted down. Expired and stale exceptions also fail. If a real
secret is ever found, rotate it first: removing it from the tree does not remove
it from Git history.

**Outstanding (SEC-110 remains open/blocking):**

- Retained success evidence for release SBOM and provenance: `release.yml`
  defines an SPDX SBOM step and a build-provenance attestation, but no
  versioned release has exercised them, and publisher identity and consumer
  verification evidence remain open
- Vulnerability-scanner integration
- Hosted exact-SHA evidence for the required `secret-scan` job
- CodeQL coverage for every shipped product
- SPDX allowlist enforcement and policy for third-party code outside
  `ThirdParty/` (for example, the editor fonts under `SparkEditor/Fonts/` have
  no license file on disk; see `THIRD_PARTY_NOTICES`)

**Stable release approval (REL-110):** stable publication runs in the protected
`stable-release` environment, whose only reviewer is the repository owner.
The release job records the run's approval history
(`.github/scripts/record_release_approval.py`) and fails unless every review is
the owner's approval for this exact run, commit, and environment, with no
rejection. The record is retained as a workflow artifact, and the independent
consumer rebuilds it from the GitHub API and requires the identical SHA-256.
An owner-only approval is a single-person review, not independent second-person
review, and no hosted stable run has yet exercised it.

The reviewed-exception schema (named owner, justification, and expiry for each
`supply-chain.lock` exception) is implemented and enforced by the checker; see
[`ThirdParty/POLICY.md`](ThirdParty/POLICY.md#reviewed-exceptions).

## Credit

We are happy to credit security researchers in the changelog and release notes. Let us know in your report how you would like to be credited.
