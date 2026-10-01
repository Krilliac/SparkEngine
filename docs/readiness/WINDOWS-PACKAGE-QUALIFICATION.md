# Windows package qualification handoff

This lane was prepared in `D:\se-s4-pkg`, branch `s4/windows-package`, based on
`4dd5aaf79c48a212ff66cf22b2b077eef8f68fdd`. Changes are left uncommitted for the
integrator. No native MSI was installed, no full C++ build or WSL command was
run, and no hosted evidence or readiness state was promoted.

## What changed

- `qualify-windows-msi.py` adds independent `repair` and `repeatability` drills.
  The required package consumer requests `interrupt,repair,repeatability`.
  Repeatability compares the complete pristine payload digest set across two
  installs, verifies both uninstalls, registration removal, absence of install
  residue, and preservation of explicitly declared external user data.
- Every native `msiexec.exe` invocation uses explicit per-user properties and
  a launcher that checks a same-user, non-elevated, medium-integrity token.
  An elevated caller uses a restricted subset of its own token, validates the
  suspended child before resuming it, and fails closed on token or cleanup
  errors, following Microsoft's [restricted-token API](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-createrestrictedtoken).
  There is no elevation fallback. WiX packages are explicitly per-user;
  MSI generation requires CMake 3.29+, while the engine's general CMake minimum
  and ZIP support are unchanged.
- `Tests/PackageSmoke/package_runtime_probe.py` verifies installed stable-v1
  asset hashes, runs the authored camera/material pixel checks, installed
  NullRHI save/reload, and AppContainer source/build denial with scene-less and
  asset-less controls. Runtime scratch is outside the checkout. The fresh CI
  consumer downloads the producer's real `CMakeCache.txt` as its build canary.
- The canonical smoke log is published only after runtime qualification and
  uninstall checks. The module-evidence consumer requires the new runtime
  fields; old lifecycle-only logs no longer pass. The runtime result expressly
  says no-display execution is unproven.
- Installer claim checks now bind preflight codes, disk budgets and their GiB
  unit, and root README flags to implementation, alongside existing flag/help,
  compile-gate and exit-code checks. Update/MinGit wording checks are lexical
  checks, not behavioral proof of arbitrary prose.
- `inventory.py --codemodel windows-release=...` aliases the canonical
  `windows-validation` profile, rejecting ambiguous aliases. The required
  Shipping producer already configures/captures all three Windows profiles.
- Readiness edits are limited to the requested criterion notes and evidence.
  Existing state fields were preserved. In particular, existing `implemented`
  states do not mean the gaps listed below have been requalified.

No C++ source or header was changed, so no C++ formatter invocation is needed.

## Integrator: real Windows configure, build and codemodel checks

Run from the integrated checkout. Substitute its path for `$repo`; retain the
current preset and toolchain pins. Use the build wrapper's RAM preflight and
do not override a STOP verdict. CMake 3.29+ and WiX must be installed for MSI
creation. The installed SDK consumer requires a completed Shipping build and
install first.

```powershell
$repo = (Get-Location).Path
$builder = 'C:\Users\Nathan\.claude\scripts\build.ps1'
powershell -NoProfile -File $builder -Dir $repo 'cmake --preset windows-shipping'
if ($LASTEXITCODE) { throw 'Shipping configure failed' }
powershell -NoProfile -File $builder -Dir $repo 'python Tools/buildmatrix/capture_provenance.py --profile windows-shipping --build-dir build/windows-shipping --build'
if ($LASTEXITCODE) { throw 'Shipping configure/build capture failed' }
cmake --install build/windows-shipping --config MinSizeRel --prefix build/build-matrix-stage
if ($LASTEXITCODE) { throw 'SDK install failed' }

powershell -NoProfile -File $builder -Dir $repo 'cmake --preset windows-release'
if ($LASTEXITCODE) { throw 'Release configure failed' }
powershell -NoProfile -File $builder -Dir $repo 'python Tools/buildmatrix/capture_provenance.py --profile windows-validation --build-dir build/windows-release --build'
if ($LASTEXITCODE) { throw 'Release configure/build capture failed' }

$versionLine = @(Get-Content build/windows-shipping/CMakeCache.txt | Select-String '^SPARK_ENGINE_VERSION:STRING=([0-9]+\.[0-9]+\.[0-9]+)$')
if ($versionLine.Count -ne 1) { throw 'Expected exactly one configured version' }
$version = $versionLine[0].Matches[0].Groups[1].Value
$sdk = Join-Path $repo 'build/build-matrix-stage/lib/cmake/SparkEngine'
$consumerConfigure = @"
cmake -S Tests/PackageSmoke -B build/installed-sdk-consumer -G "Visual Studio 17 2022" -A x64 -T v143 -DSparkEngine_DIR="$sdk" -DSPARK_EXPECTED_ENGINE_VERSION=$version
"@
powershell -NoProfile -File $builder -Dir $repo $consumerConfigure
if ($LASTEXITCODE) { throw 'SDK consumer configure failed' }
powershell -NoProfile -File $builder -Dir $repo 'python Tools/buildmatrix/capture_provenance.py --profile installed-sdk-consumer --build-dir build/installed-sdk-consumer --build'
if ($LASTEXITCODE) { throw 'SDK consumer configure/build capture failed' }
ctest --test-dir build/installed-sdk-consumer -C Release --output-on-failure --no-tests=error
if ($LASTEXITCODE) { throw 'SDK consumer tests failed' }

python Tools/buildmatrix/inventory.py `
  --codemodel windows-shipping=build/windows-shipping `
  --codemodel windows-release=build/windows-release `
  --codemodel installed-sdk-consumer=build/installed-sdk-consumer `
  --output build/pkg-configured-inventory.json
if ($LASTEXITCODE) { throw 'Configured inventory failed' }
python Tools/buildmatrix/check_parity.py --inventory build/pkg-configured-inventory.json --output build/pkg-configured-parity.json
```

`capture_provenance.py` owns a new File API query and configure transaction; it
does not relabel an old reply. It reconfigures and builds when `--build` is
given. A local parity report can still reject missing protected authority; do
not change that gate or treat a structural inventory as a hosted attestation.

Record the staged Shipping dependency closure independently:

```powershell
$python = (Get-Command python).Source
cmake "-DSPARK_ENGINE_BUILD_DIR=$repo/build/windows-shipping" `
  "-DSPARK_SOURCE_ROOT=$repo" -DSPARK_CONFIG=MinSizeRel `
  "-DSPARK_TEST_ROOT=$env:TEMP/spark-shipping-closure" `
  "-DSPARK_PYTHON_EXECUTABLE=$python" `
  -P Tests/PackageSmoke/VerifyWindowsPackageClosure.cmake
if ($LASTEXITCODE) { throw 'Shipping dependency closure failed' }
```

The required `build-windows-shipping` job already runs this driver against both
Windows row plans and uploads `shipping-closure-<sha>`. It measures a fresh
stage of the MSI component set; it is not a physical Windows 11 certification
record and does not itself execute Windows Installer.

## Integrator: real MSI drills

Use a disposable, non-elevated Windows user session with no related SparkEngine
MSI registered. The qualifier refuses an existing/related product. Do not
install into Program Files, write HKLM, or use a SYSTEM task. Native commands
remain confined to a new temporary root. Run this on a completed Shipping tree:

```powershell
$run = Join-Path $env:TEMP ('spark-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$packages = Join-Path $run 'packages'
$sha = (git rev-parse HEAD).Trim()
cpack --config build/windows-shipping/CPackConfig.cmake -G WIX -C MinSizeRel -B $packages
if ($LASTEXITCODE) { throw 'MSI packaging failed' }
python .github/scripts/write-shipping-package-manifest.py --packages $packages --version $version `
  --commit-sha $sha --out "$packages/shipping-package-manifest.json"
if ($LASTEXITCODE) { throw 'MSI identity manifest failed' }

# Run each drill individually, then the same combined sequence as required CI.
# Every call also performs runtime validation and the final uninstall.
foreach ($drill in @('interrupt', 'repair', 'repeatability', 'interrupt,repair,repeatability')) {
  $logs = Join-Path $run ('logs-' + $drill.Replace(',', '-'))
  python .github/scripts/qualify-windows-msi.py `
    --packages $packages --version $version `
    --manifest "$repo/build/windows-shipping/SparkEngineGameModules.cmake" `
    --package-manifest "$packages/shipping-package-manifest.json" `
    --build-root "$repo/build/windows-shipping" `
    --runner-temp $run --logs $logs --source-sha $sha --drills $drill
  if ($LASTEXITCODE) { throw "MSI drill failed: $drill; retain $run for diagnosis" }
}
```

Do not describe an MSI built from dirty source as exact-commit evidence. After
integration, rebuild from the commit whose SHA is written to the manifest.
The plain `repair` drill has no predecessor requirement. `--bootstrap-repair`
still requires its reviewed baseline. N-1 upgrade interruption still requires
the genuine signed, receipted predecessor; no synthetic substitute was added.

## Integrator: Python contracts and Linux verification

On Windows, rerun these without the current restricted temporary-directory
failure. Keep failure exit codes and preserve complete logs:

```powershell
python .github/scripts/test_qualify_windows_msi.py
python .github/scripts/test-run-non-elevated-windows.py
python Tests/PackageSmoke/package_runtime_probe_tests.py
python Tests/PackageSmoke/windows_appcontainer_run_tests.py
python Tests/Tools/test_package_smoke_artifacts.py
python Tests/Tools/test_module_evidence.py
python Tests/Tools/test_installer_claims.py
python Tests/Tools/test_windows_package_closure.py
python Tests/Tools/test_build_matrix_parity.py
python .github/scripts/test-workflow-failure-propagation.py
python tools/installer/check_installer_claims.py
python tools/site-data/validate.py
python tools/site-data/render_handoff.py --check
```

In an already provisioned WSL checkout with the repository's supported GCC and
dependencies, use the following Linux commands. These were not run in this
lane; WSL is unavailable here. Windows MSI/AppContainer execution remains a
Windows-only integration step.

```bash
cd /mnt/d/se-s4-pkg
python3 .github/scripts/test_qualify_windows_msi.py
python3 .github/scripts/test-run-non-elevated-windows.py
python3 Tests/PackageSmoke/package_runtime_probe_tests.py
python3 Tests/Tools/test_package_smoke_artifacts.py
python3 Tests/Tools/test_module_evidence.py
python3 Tests/Tools/test_installer_claims.py
python3 Tests/Tools/test_build_matrix_parity.py
python3 Tests/Tools/test_windows_package_closure.py
python3 .github/scripts/test-workflow-failure-propagation.py
cmake --preset linux-gcc-release
cmake --build build/linux-gcc-release --parallel 2
ctest --test-dir build/linux-gcc-release --output-on-failure --no-tests=error -R '^Installer_(DocClaims|Interrupted_Msi|Repair_Msi|Repeatability_Msi|Uninstall|Upgrade|Rollback)$'
```

## Remaining qualification and owner decisions

- The new package/runtime/MSI paths have not executed against a real MSI in this
  lane. The elevated-runner restricted-token path needs native hosted proof;
  the local medium-token child smoke is a different path.
- No-desktop execution, an enumeration of owned windows, and display-adapter
  absence remain unproven. The runtime helper deliberately emits no PASS claim
  for them. Existing OS D3D imports are not themselves evidence of GPU use.
- There is no added Windows forced-termination/recovery drill, Windows process
  sanitizer lane, or FPS Windows soak. The owner must decide whether Windows
  sanitizer execution is required or whether shared Linux coverage is an
  accepted scope change; this lane did not make that decision.
- Runtime model/material/texture load totals and a terminal zero-failure count
  are not currently emitted. Authored scene/material pixel mutation and
  AppContainer negative controls are the actual proofs wired here.
- Windows 11, NSIS, SparkInstaller reuse and signed predecessor qualification
  require their actual supported environments/artifacts. No criterion text,
  budget, threshold, exemption, or signing gate was narrowed.
- The owner still decides whether PLT-200[2] accepts the recorded Shipping
  component closure or also requires a closure probe inside each physical
  certification record. The code evidence supports the former measurement;
  this lane did not amend OD-27 or declare the criterion evidenced.

The local command results and any sandbox-blocked checks are recorded in the
final lane report; full validation output is retained under `build/pkg-validation`.
