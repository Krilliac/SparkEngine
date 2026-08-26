# Spark CLI

`spark_cli.py` creates, builds, runs, validates, migrates, and packages standalone SparkEngine projects.
Run it from a project root containing `CMakeLists.txt`, one `*.sparkproject` descriptor, and
`spark.modules.json`.

## Run a project

```powershell
python <engine-root>/Tools/spark-cli/spark_cli.py run --config Release
```

`spark run` builds the selected configuration and then launches the freshly built module with a
matching SparkEngine host. If the engine checkout has no matching host, a validated host from the
SparkEditor package is used. It supports both normal CLI build trees and the runnable package generated
by SparkEditor:

1. A normal build resolves every declared module under `build/<config>` or `build`. Single-module projects
   use `-game <module>`; multi-module projects use the Windows host's explicit `-manifest` input. POSIX hosts
   without that switch receive a temporary, invocation-owned runnable directory containing exactly those
   modules. A stale packaged module is never preferred over the build that just completed.
2. With `--no-build`, `build/Output` (or `build/<config>/Output`) is launched directly after its module
   manifest, module binaries, ABI sidecars, and runtime host have been validated.

Use an already-built package without compiling again:

```powershell
python <engine-root>/Tools/spark-cli/spark_cli.py run --config Release --no-build
```

Select an existing package explicitly (which also skips the build) and forward runtime arguments after `--`:

```powershell
python <engine-root>/Tools/spark-cli/spark_cli.py run --no-build --package build/Output -- -no-splash -test-frames 3
```

Set `SPARKENGINE_RUNTIME_HOST` to an explicit host executable when running a module from a build tree
outside the engine checkout. Set `SPARK_ENGINE_DIR` to the engine root when auto-discovery cannot find
it. POSIX hosts must have an executable permission bit. The CLI waits for the game process and returns its
exit code.

Package manifests are treated as untrusted input: module paths must stay inside the package, and each
module must have its pre-load ABI sidecar. Extra root or nested `path` keys are rejected because older
runtime manifest readers interpret every such key as a module. Ambiguous project descriptors, module
outputs, or runtime hosts fail with an actionable error instead of launching a stale binary.

## Package a project

```powershell
python <engine-root>/Tools/spark-cli/spark_cli.py package --config Release --output dist
```

The command builds the selected configuration and creates
`dist/<project>-<platform>-<config>`. Unlike the legacy binary sweep, the result is a validated
runnable-package layout containing:

- `SparkGame.exe` (`SparkGame` on POSIX), every declared module, and each module's `.sparkabi` sidecar;
- a generated `spark.modules.json` that preserves root and per-module metadata while rewriting module paths
  to their packaged filenames;
- runtime `Shaders`, optional `Resources`, and engine branding assets;
- project `Assets`, `Scenes`, `Config`, and the active project descriptor;
- `Startup.sparkscene` plus an isolated scene-preview host when a startup scene exists;
- native game/scene launchers, package guidance, and a `manifest.json` whose entrypoint is the game launcher
  with `workingDirectory` set to the package root.

Packaging rejects cross-platform requests without a matching native toolchain, ambiguous module outputs,
unsafe project names, linked content that escapes the project/runtime roots, and output paths inside live
`Assets`, `Scenes`, or `Config`. It also rejects output that overlaps the project root, active build tree,
or runtime-host source directory. Final package paths that are symlinks, junctions, or reparse points are
never replaced. A package carrying Spark CLI ownership metadata is replaced normally; an existing unknown
directory requires explicit `--force`, which still never permits replacing a link.

Assembly uses a marked, invocation-owned transaction directory and moves an older package aside only after
every required artifact has been staged. Publishing uses two same-volume renames, so it is transactional
rather than strictly atomic: a failed second rename restores the previous package, while a failed restore
preserves the recovery directory and prints its location. On startup, a missing final package is recovered
only when exactly one matching transaction contains a validated, owned previous package. Ambiguous, stale,
foreign, and unowned recovery data is preserved for manual inspection. Pre-existing legacy staging or backup
directories are never deleted. Project `defaultScene` accepts either slash style while remaining confined to
the project root.

The package contains SparkEngine-owned runtime files, but it does not vendor platform-installed dynamic
libraries or optional companion tools such as the external console and crash reporter. Distribute those
dependencies according to the target platform's deployment policy.

`--strip` remains accepted and omits external PDB files, but does not mutate module bytes because doing so
would invalidate the pre-load ABI hash. `--compress` also remains accepted; the current runnable-package
contract keeps assets raw and records both requested and effective states in `manifest.json`.

## Inspect SparkPak archives

```powershell
python <engine-root>/Tools/spark-cli/spark_cli.py pak inspect Data/base.spk --format json
python <engine-root>/Tools/spark-cli/spark_cli.py pak list Data/base.spk
python <engine-root>/Tools/spark-cli/spark_cli.py pak verify Data/base.spk --format json
python <engine-root>/Tools/spark-cli/spark_cli.py pak diff Data/base.spk Data/patch.spk
```

These commands are strictly read-only. They validate bounded header, table-of-contents, path, hash, size,
and data-range contracts without extracting files. `verify` streams and SHA-256 hashes stored and deflate
entries; `diff` compares those decompressed hashes. Zstd entries remain visible to `inspect` and `list`,
while verification fails explicitly when no optional zstd provider is available.

## Focused tests

```powershell
python -m unittest discover -s Tools/spark-cli/tests -p "test_*.py" -v
```
