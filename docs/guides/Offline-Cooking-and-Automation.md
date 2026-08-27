# Offline Cooking and Runtime Automation

SparkEngine ships four small headless tools for reproducible content builds and
black-box runtime checks. They are controlled by `ENABLE_ASSET_PIPELINE_TOOLS`
and `ENABLE_AUTOMATION_HOST` in the root CMake configuration.

## Deterministic asset cooking

```powershell
SparkCooker --source Assets --output Cooked/Assets
SparkCooker --source Assets --output Cooked/Assets --dry-run
SparkCooker --source Assets --output Cooked/Assets `
  --worker build/bin/Release/SparkWorker.exe --jobs 4
```

`SparkCooker` walks regular files in normalized lexical order, rejects linked
roots and source/output overlap, computes SHA-256 without loading whole files,
and mirrors content into the output tree. Existing outputs with the same digest
are left untouched. `spark-cook-manifest.json` contains stable paths, sizes, and
digests plus a manifest digest derived only from that canonical content—not
timestamps or machine-specific absolute paths. It is published at the root of
the cooked asset tree (`Cooked/Assets/spark-cook-manifest.json` in editor packages).

`SparkAssetPipelineCore` owns these reusable operations. Format-specific
processors can be layered on it later without putting editor UI code in the
headless pipeline.

With `--worker`, the cooker becomes the bounded scheduler while each
`SparkWorker` retains authority over exactly one digest-pinned job. `--jobs`
selects 1–64 concurrent lanes. Worker output is written to an isolated scratch
generation, then the cooker re-hashes and verifies the complete scheduled set
before the existing atomic manifest/publication path runs. A failed worker,
missing output, digest mismatch, unexpected extra file, or linked scratch output
aborts the generation and preserves the previously published cook. `--dry-run`
still dispatches digest-pinned workers but publishes nothing.

The editor's **Build & Cook → Cook Only** action launches this same executable
and streams its `[current/total]` output into the panel. Scenes, configuration,
project metadata, and the module manifest are packaged alongside the cooked
asset tree. Cancelling the panel operation terminates the complete cooker
process tree rather than leaving workers behind.

The full **Build** action is deliberately native-only: it configures and builds
the project module, requires the matching `.sparkabi` sidecar, assembles the
native SparkEngine host and shared-library dependencies, rewrites
`spark.modules.json`, and then runs SparkCooker into the package. Windows emits
`.cmd` launchers; Linux and macOS emit executable `.sh` launchers. A non-native
target or missing host/module/sidecar/cooker fails the build instead of reporting
success with an incomplete package.

The dedicated-server panel uses that same complete flow and additionally builds
and packages SparkServer plus `LaunchServer.cmd` or `LaunchServer.sh`. The server
therefore ships beside the exact game module, ABI sidecar, cooked content,
rewritten manifest, shaders, resources, and native runtime dependencies used by
the playable package.

## Isolated worker jobs

```powershell
SparkWorker --source Assets/mesh.glb --output Cooked/Assets/mesh.glb `
  --sha256 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
```

`SparkWorker` performs one bounded copy job. It refuses the job unless the
source digest exactly matches the scheduler-provided digest, making stale or
misrouted distributed work visible. `--dry-run` reports the incremental
decision without writing.

## Black-box runtime automation

```powershell
SparkAutomation --name template-smoke --executable build/bin/Release/SparkEngine.exe `
  --working-dir Packages/MyGame --frames 120 --timeout-ms 30000 --expected-exit 0 `
  --captured-log TestResults/template.log --log-contains "initialized" `
  --screenshot Screenshots/final.png --json TestResults/template.json `
  --junit TestResults/template.xml -- -headless -manifest spark.modules.json
```

Arguments after `--` go to the runtime. A nonzero result means launch failure,
timeout, unexpected exit code, missing log text, or a missing/empty screenshot.
Expected screenshot paths are cleared before launch only when they are regular
files; directories and symbolic links are refused, so stale output cannot make
a run pass.
JSON is intended for detailed artifacts and JUnit for CI ingestion. On Windows,
the runtime is placed in a kill-on-close Job Object; on POSIX it receives its
own process group. The POSIX timeout path also freezes and enumerates descendants
that created a new process group or session before terminating the complete
tree; Linux additionally adopts orphaned grandchildren as a subreaper. A
timeout therefore terminates escaped descendants as well as the immediate host.

The editor packaging panel can enable **Run packaged game smoke test**. After
a successful native package, the editor launches `SparkAutomation` with the
selected frame and timeout bounds. It writes `Automation/runtime.log`,
`Automation/result.json`, and `Automation/junit.xml` inside the package; a
launch failure, timeout, or unexpected exit fails the editor build.

## Read-only SparkPak diagnostics

```powershell
python Tools/spark-cli/spark_cli.py pak inspect Data/base.spk --format json
python Tools/spark-cli/spark_cli.py pak list Data/base.spk
python Tools/spark-cli/spark_cli.py pak verify Data/base.spk --format json
python Tools/spark-cli/spark_cli.py pak diff Data/base.spk Data/patch.spk
```

These commands never extract or modify content. They reject linked inputs,
malformed/truncated TOCs, unsafe or duplicate virtual paths, hash mismatches,
overlapping data ranges, unknown compression, and configured size-limit
violations. Listing is sorted. Verification streams decompressed bytes into
SHA-256; stored and deflate entries use the Python standard library. Zstd
metadata can be inspected, but verification fails explicitly unless a future
provider is added. `pak diff` returns 0 for identical content, 1 for a valid
content difference, and 3–5 for archive, compression, or I/O errors.
