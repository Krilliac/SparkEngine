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

1. A normal build finds the configured module under `build/<config>` or `build` and launches it with
   `-game <module>` and `-project <descriptor>`, so a stale packaged module is never preferred over the
   build that just completed.
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
it. The CLI waits for the game process and returns its exit code.

Package manifests are treated as untrusted input: module paths must stay inside the package, and each
module must have its pre-load ABI sidecar. Ambiguous project descriptors, module outputs, or runtime
hosts fail with an actionable error instead of launching a stale binary.

## Focused tests

```powershell
python -m unittest discover -s Tools/spark-cli/tests -p "test_*.py" -v
```
