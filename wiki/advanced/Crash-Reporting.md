# Crash Reporting

Spark's crash path writes a local manifest and artifacts, then launches `SparkCrashReporter` to present them to the user. The reporter remains **read-only for crash artifacts**: it displays local information but does not modify or upload the log, dump, screenshot, or archive. A separate, explicit user-local opt-in can post a small metadata-only GitHub Issue through the user's already-authenticated GitHub CLI. Watchdog queue housekeeping is narrower but mutating: it atomically renames a ready manifest to a claimed name and consumes that claimed manifest after loading it.

**Sources:** `SparkEngine/Source/Utils/CrashHandler.cpp`, `SparkCrashReporter/src/CrashReporterApp.cpp`

## Trust boundary

The crash directory is an authoritative local root, not a hint. Runtime code opens that directory without following its final symlink/reparse point, opens immediate child names relative to the pinned root, rejects non-regular files and hard links, and compares file identities before later use. Artifact references are filenames in the root; nested or escaping paths are invalid.

Ready manifests use `crash_manifest_<16 lowercase hex>.json`. The reporter bounds a manifest at 1 MiB, JSON strings at 256 KiB, nesting at 16, collections at 4,096 entries, the ready queue at 32 manifests, and a displayed crash log at 8 MiB. These are source constants, not claims about delivery or retention.

Legacy transport fields (`uploadURL`, proxy, GitHub, SMTP, and email fields) are parse-only compatibility input. They are discarded and must not be written into new manifests. `requireConsent: false` also does **not** authorize network delivery. `artifactRoot` and pinned identities are in-memory trust state and must not be serialized.

## Offline validation

The repository includes read-only tools under `tools/ops`:

```text
python tools/ops/validate_crash_package.py --writer-output --check-names <crash-root>
python tools/ops/redact_secrets.py <crash-root>
```

The validator opens the supplied root and files through pinned handles; rejects symlinks, junctions/reparse points, hard links, Windows/Unix absolute paths, alternate-data-stream spellings, traversal, and filename aliases before file content is read; validates referenced artifacts; and applies bounded aggregate, count, and time policies. It never deletes or modifies the package. `--check-names` checks manifest names encountered while scanning a supplied root, not just a directly named file.

Secret inspection is shared with telemetry validation. Text, JSON values, dump byte strings, UTF-16 dump strings, and bounded ZIP members are inspected. Opaque image/PDF artifacts and unsupported containers fail closed until a format-aware inspection policy exists. ZIPs are inspected in memory without extraction and reject traversal, encryption, nested archives, excessive expansion, and excessive compression ratios.

The aggregate/file/archive/time limits defined by the Python tools are defensive offline-validation limits. They are **not C++ runtime guarantees**.

## Optional automatic GitHub Issues

From the installed `bin` directory, a playtester who wants public, automatic **metadata-only** Issues can run:

```text
SparkCrashReporter --enable-auto-issues
SparkCrashReporter --auto-issues-status
SparkCrashReporter --disable-auto-issues
SparkCrashReporter --issue-status <private-crash-directory>
```

Opt-in is off by default, persists in the user's local configuration, and is revocable. It requires a trusted `gh` executable on an absolute `PATH` entry and the playtester's own authenticated GitHub account with permission to create Issues in `Krilliac/SparkEngine`. The reporter invokes `gh issue create` without a shell, PAT, or embedded token; it cannot verify the provenance of a binary on the user's `PATH`. The target repository is fixed; a crash manifest cannot redirect it. The Issue contains only reporter version, platform, and an opaque incident ID. No log text, dump, screenshot, file path, command line, crash title, or user description is transmitted. **GitHub Issues are public**; opt in only if publishing that limited metadata is acceptable.

The reporter first validates and reviews local crash artifacts. Declining an interactive review prevents the Issue attempt. Missing `gh` and other certain local setup failures do not claim the incident, so it can be retried after setup. Immediately before contacting GitHub, it writes a one-attempt receipt in the private crash directory; a timeout or lost response is never retried automatically into a duplicate public Issue. The reporter also saves `confirmed` plus the exact Issue URL, or `unconfirmed`, in a bounded local result file. `--issue-status` reads those receipts even when the detached watchdog's output is hidden. Authentication or network failures, timeouts, and unexpected replies are unconfirmed; local artifacts remain available. To report manually, review and sanitize the evidence first, then use the repository's Issues tab.

The automatic Issue is only a signal, not a triage-ready bug report. The playtester should add sanitized reproduction steps and an exact build identifier when known, without pasting raw logs, dumps, screenshots, private paths, tokens, or personal data. This feature is **not crash-artifact upload** and does not satisfy the OPS-100 relay/symbolication gate. A future private relay still needs scoped ephemeral authorization, explicit reversible consent, truthful delivery states, end-to-end tests, private symbol publication, and a release-crash canary.

## Remaining work

- Controlled relay and scoped ephemeral authorization
- Explicit upload consent and delivery state machine
- Private symbol publication plus release-crash symbolication canary
- CI jobs and reviewed privacy/retention/runbook evidence
