# Crash Reporting

Spark's crash path writes a local manifest and artifacts, then launches `SparkCrashReporter` to present them to the user. The current reporting operation is **read-only for crash artifacts**: it displays local information but does not modify the log, dump, screenshot, or archive. It does not upload, email, open a GitHub issue, contact a relay, or consume reusable network credentials. Watchdog queue housekeeping is narrower but mutating: it atomically renames a ready manifest to a claimed name and consumes that claimed manifest after loading it.

**Sources:** `SparkEngine/Source/Utils/CrashHandler.cpp`, `SparkCrashReporter/src/CrashReporterApp.cpp`

## Trust boundary

The crash directory is an authoritative local root, not a hint. Runtime code opens that directory without following its final symlink/reparse point, opens immediate child names relative to the pinned root, rejects non-regular files and hard links, and compares file identities before later use. Artifact references are filenames in the root; nested or escaping paths are invalid.

Ready manifests use `crash_manifest_<16 lowercase hex>.json`. The reporter bounds a manifest at 1 MiB, JSON strings at 256 KiB, nesting at 16, collections at 4,096 entries, the ready queue at 32 manifests, and a displayed crash log at 8 MiB. These are source constants, not claims about delivery or retention.

Legacy transport fields (`uploadURL`, proxy, GitHub, SMTP, and email fields) are parse-only compatibility input. They are discarded and must not be written into new manifests. `artifactRoot` and pinned identities are in-memory trust state and must not be serialized.

## Offline validation

The repository includes read-only tools under `tools/ops`:

```text
python tools/ops/validate_crash_package.py --writer-output --check-names <crash-root>
python tools/ops/redact_secrets.py <crash-root>
```

The validator opens the supplied root and files through pinned handles; rejects symlinks, junctions/reparse points, hard links, Windows/Unix absolute paths, alternate-data-stream spellings, traversal, and filename aliases before file content is read; validates referenced artifacts; and applies bounded aggregate, count, and time policies. It never deletes or modifies the package. `--check-names` checks manifest names encountered while scanning a supplied root, not just a directly named file.

Secret inspection is shared with telemetry validation. Text, JSON values, dump byte strings, UTF-16 dump strings, and bounded ZIP members are inspected. Opaque image/PDF artifacts and unsupported containers fail closed until a format-aware inspection policy exists. ZIPs are inspected in memory without extraction and reject traversal, encryption, nested archives, excessive expansion, and excessive compression ratios.

The aggregate/file/archive/time limits defined by the Python tools are defensive offline-validation limits. They are **not C++ runtime guarantees**.

## Consent and delivery status

There is no crash upload implementation to consent to today. A future relay must use scoped, ephemeral authorization, explicit reversible consent, truthful queued/delivered/rejected/failed states, and end-to-end tests. Until a synthetic release crash reaches that relay and symbolicates against private build-ID-indexed symbols, OPS-100 remains open and crash delivery must not be presented as available.

## Remaining work

- Controlled relay and scoped ephemeral authorization
- Explicit upload consent and delivery state machine
- Private symbol publication plus release-crash symbolication canary
- CI jobs and reviewed privacy/retention/runbook evidence
