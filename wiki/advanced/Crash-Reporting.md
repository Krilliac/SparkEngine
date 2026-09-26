# Crash Reporting

Spark's crash path writes a local manifest and artifacts, then launches `SparkCrashReporter` to present them to the user. The reporter remains **read-only for crash artifacts**: it displays local information but does not modify or upload the log, dump, screenshot, or archive. A separate, explicit user-local opt-in can post a small metadata-only GitHub Issue through the user's already-authenticated GitHub CLI. Watchdog queue housekeeping is narrower but mutating: it atomically renames a ready manifest to a claimed name and consumes that claimed manifest after loading it.

**Sources:** `SparkEngine/Source/Utils/CrashHandler.cpp`, `SparkCrashReporter/src/CrashReporterApp.cpp`

## Trust boundary

The crash directory is an authoritative local root, not a hint. Runtime code opens that directory without following its final symlink/reparse point, opens immediate child names relative to the pinned root, rejects non-regular files and hard links, and compares file identities before later use. Artifact references are filenames in the root; nested or escaping paths are invalid.

Ready manifests use `crash_manifest_<16 lowercase hex>.json`. The reporter bounds a manifest at 1 MiB, JSON strings at 256 KiB, nesting at 16, collections at 4,096 entries, the ready queue at 32 manifests, and a displayed crash log at 8 MiB. These are source constants, not claims about delivery or retention.

Legacy transport fields (`uploadURL`, proxy, GitHub, SMTP, and email fields) are parse-only compatibility input. They are discarded and must not be written into new manifests. `requireConsent: false` also does **not** authorize network delivery. `artifactRoot` and pinned identities are in-memory trust state and must not be serialized.

The engine side accepts no transport configuration at all. `CrashConfig` and `[CrashReporting]` have no upload URL, relay URL, GitHub token/repository, SMTP credential, or e-mail field, and `SetupCrashHandler` reads no credential environment variables. The former in-process uploader (`CrashReportUploader.cpp`) was deleted because nothing called it. `EngineSettings::Load` drops the retired `[CrashReporting]` keys (`UploadURL`, `ProxyURL`, `GitHubRepo`, `GitHubToken`, `GitHubLabels`, `AttachDump`, `TimeoutSeconds`, `SmtpUser`, `SmtpPass`, `EmailTo`, `EmailFrom`, in any letter case) from `settings.ini` and `settings.local.ini`, so a later `Save()` cannot write a stale secret back. Regressions: `CrashSettings_RetiredTransportCredentialsAreDroppedOnLoadAndNeverWritten` (C++) and the source/settings policy checks in `Tests/Tools/test_ops100_crash_security.py`.

## Offline validation

The repository includes read-only tools under `tools/ops`:

```text
python tools/ops/validate_crash_package.py --writer-output --check-names <crash-root>
python tools/ops/redact_secrets.py <crash-root>
```

The validator opens the supplied root and files through pinned handles; rejects symlinks, junctions/reparse points, hard links, Windows/Unix absolute paths, alternate-data-stream spellings, traversal, and filename aliases before file content is read; validates referenced artifacts; and applies bounded aggregate, count, and time policies. It never deletes or modifies the package. `--check-names` checks manifest names encountered while scanning a supplied root, not just a directly named file.

Secret inspection is shared with telemetry validation. Text, JSON values, dump byte strings, UTF-16 dump strings, and bounded ZIP members are inspected. Opaque image/PDF artifacts and unsupported containers fail closed until a format-aware inspection policy exists. ZIPs are inspected in memory without extraction and reject traversal, encryption, nested archives, excessive expansion, and excessive compression ratios.

The aggregate/file/archive/time limits defined by the Python tools are defensive offline-validation limits. They are **not C++ runtime guarantees**.

`ctest -L crash-security` includes `CrashCapturePackageSecurity` on every native host that builds `SparkCrashReporter` with miniz (Windows and POSIX). Cross-compiles are excluded, and so are builds without miniz, because those link `CrashHandlerStub.cpp`, which produces no artifacts. The hosted `security-runtime` job selects this label on Linux Shipping. That configuration has not been run locally and has no hosted record yet. `Tests/Tools/run_crash_capture_security.py` runs the production producer test `CrashHandler_UngatedReportWritesAnArtifactAndTheAssertGateDoesNot` in an isolated temporary root, then feeds its untouched artifacts to the native reporter (`--report`) and to `validate_crash_package.py`, and checks both leave bytes and file identities unchanged. It also checks a traversal copy and a legacy-transport-field copy. On Windows the producer must emit a log and a minidump. On POSIX the ungated assertion report is log-only, and its manifest must carry an empty `dumpFile`. Signal-crash core files belong to the kernel `core_pattern` and are not referenced. The POSIX test saves the suite's crash-signal handlers and restores them afterwards. On POSIX the runner removes its temporary work root afterwards, through a directory descriptor pinned to the root's recorded identity. It does not follow symlinks. A root that was replaced is kept, not deleted. On Windows the root is kept and its path is printed, because deletion there cannot be bound to a pinned identity.

## Linux build-id symbolication (local canary)

On Linux, `InstallCrashHandler()` walks the loaded ELF modules with `dl_iterate_phdr` and records each module's GNU build-id (`NT_GNU_BUILD_ID`), load bias, `PT_LOAD` address range and basename in fixed-capacity storage (`Utils/CrashSymbolication.h`). `ModuleManager` calls `RefreshCrashModuleIdentities()` after each successful `dlopen`, so game-module frames are covered too. It refreshes again after a module is unloaded or a load is rejected after `dlopen`, so an unmapped module's range is never matched. Libraries that other code opens later are not recorded: Vulkan and GL driver ICDs, and SDL or audio plugins, are opened by their loaders during backend initialisation. Frames in them are written as `module=- address=` and are not symbolicated. This gap is tracked in OPS-100. A refresh fills the inactive one of two tables and then publishes it with one atomic store. The signal handler therefore only reads precomputed data. It takes the exact faulting PC from the signal context and formats the section into a static buffer, with no allocation, ELF parsing or loader call. The rest of the POSIX report is still best-effort heap work, as before. The section is appended to every Linux crash and assertion log:

```text
*** SYMBOLIC FRAMES ***
MODULE <index> build_id=<hex> bias=0x<hex> name=<basename>
SYMFRAME <n> kind=<pc|ra> module=<index> offset=0x<hex>
SYMFRAME <n> kind=<pc|ra> module=- address=0x<hex>
*** END SYMBOLIC FRAMES ***
```

Only basenames are written, never absolute module paths. `kind=pc` is the exact faulting instruction; `kind=ra` is a return address, which the symbolizer steps back by one byte into the call instruction.

`tools/ops/symbolicate_crash.py` resolves the section offline against a local store in the debuginfod layout, `<store>/.build-id/<xx>/<rest>.debug`:

```text
python3 tools/ops/symbolicate_crash.py store   --store <dir> <binary>...
python3 tools/ops/symbolicate_crash.py resolve --store <dir> --log <crash.log> [--json]
```

`store` splits debug info with `objcopy --only-keep-debug` and files it under the binary's build-id. It refuses a binary with no build-id or no DWARF (`-g`), and it never overwrites an existing entry. `resolve` bounds the log (8 MiB, 512 modules, 256 frames) and accepts only the exact record grammar. It opens store entries without following symlinks, and then runs `addr2line` on the pinned descriptor. A store entry whose own build-id differs from the recorded one is refused with exit 2. The tool does not return plausible but wrong lines. A module with no store entry is reported as `missing-symbols`, never guessed.

`ctest -R CrashReporter_Symbolication` (label `crash-canary`) runs the canary: `SparkCrashSymbolicationProbe` (`Tests/Fixtures/CrashSymbolicationProbe.cpp`, built `-g -O2 -Wl,--build-id=sha1`) installs the production handler under an isolated `TMPDIR` and faults. `Tests/Tools/run_crash_symbolication_canary.py` requires death by SIGSEGV, stores the probe's split debug info, and requires frame 0 to resolve to `SparkSymbolicationCanaryCrashSite` at the marked source line. It also requires a return-address frame to resolve to `main`, and it checks that a wrong build-id and a symlinked store entry are both refused. `CrashSymbolicationRecords` (`Tests/TestCrashSymbolication.cpp`) covers the bounded note parser and the section format, including whole-line truncation. `Tests/Tools/test_ops100_symbolication.py` runs in the `validate-ops100` job and covers the tool's grammar, bounds and store policy.

What this does not prove: there is no relay or upload and no private symbol publication. Shipping builds now produce the symbols this store needs (BLD-100): with `STRIP_DEBUG_SYMBOLS=ON` every ELF image is compiled with `-g`, linked with a SHA-1 build-id and split at link time into a stripped image and `<image>.debug`, which installs only into the unpackaged `symbols` component (see [CI Reproducible Builds](../development/CI-Reproducible-Builds.md#shipping-private-symbols-bld-100)). Builds without `STRIP_DEBUG_SYMBOLS` are unchanged. Windows PDB/minidump symbolication is not covered, and no `crash-canary` CI job or hosted record exists yet.

## Optional automatic GitHub Issues

From the installed `bin` directory, a playtester who wants public, automatic **metadata-only** Issues can run:

```text
SparkCrashReporter --enable-auto-issues
SparkCrashReporter --auto-issues-status
SparkCrashReporter --disable-auto-issues
SparkCrashReporter --issue-status <private-crash-directory>
```

Opt-in is off by default, persists in the user's local configuration, and is revocable. It requires a trusted `gh` executable on an absolute `PATH` entry and the playtester's own authenticated GitHub account with permission to create Issues in `Krilliac/SparkEngine`. The reporter invokes `gh issue create` without a shell, PAT, or embedded token; it cannot verify the provenance of a binary on the user's `PATH`. The target repository is fixed; a crash manifest cannot redirect it. The Issue contains only reporter version, platform, an opaque incident ID, and a crash class selected from fixed engine-generated titles. Any other title becomes `Unknown`; the manifest's free-form title is never echoed. No log text, dump, screenshot, file path, command line, or user description is transmitted. **GitHub Issues are public**; opt in only if publishing that limited metadata is acceptable.

The reporter first validates and reviews local crash artifacts. Declining an interactive review prevents the Issue attempt. Missing `gh` and other certain local setup failures do not claim the incident, so it can be retried after setup. Immediately before contacting GitHub, it writes a one-attempt receipt in the private crash directory; a timeout or lost response is never retried automatically into a duplicate public Issue. The reporter also saves `confirmed` plus the exact Issue URL, or `unconfirmed`, in a bounded local result file. Interactive Windows reviews show the outcome in a dialog because the watchdog runs without a console; `--issue-status` reads the receipts later or after noninteractive runs. Authentication or network failures, timeouts, and unexpected replies are unconfirmed; local artifacts remain available. If `gh` is missing, the reporter shows an incident ID and a manual Issues link. Review and sanitize any evidence before posting it there.

The automatic Issue is only a signal, not a triage-ready bug report. The playtester should add sanitized reproduction steps and an exact build identifier when known, without pasting raw logs, dumps, screenshots, private paths, tokens, or personal data. This feature is **not crash-artifact upload** and does not satisfy the OPS-100 relay/symbolication gate. A future private relay still needs scoped ephemeral authorization, explicit reversible consent, truthful delivery states, end-to-end tests, private symbol publication, and a release-crash canary.

## Remaining work

- Controlled relay and scoped ephemeral authorization
- Explicit upload consent and delivery state machine
- Private symbol publication and a release-crash canary through a relay (Shipping builds produce split symbols and a build-id manifest; the local Linux build-id canary above covers symbolication only)
- CI jobs and reviewed privacy/retention/runbook evidence
