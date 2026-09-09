# MOD-310 Packaged Runtime Evidence Design

## Purpose

Close the `package-smoke-log` evidence gap with a real clean-install smoke of
the `windows-shipping` (`MinSizeRel`) stable-v1 package.  This makes the
existing module-evidence gate consume a package observation rather than a
declared future test.

It does **not** mark MOD-310 complete.  The FPS module still uses private
engine build inputs, and the installed single-player/save acceptance work is
separate and remains visibly open.

## Evidence that motivates the change

- `build-windows-shipping` is the authoritative stable-v1 Windows build:
  `windows-shipping`, `MinSizeRel`, and only `SparkGameFPS`.
- It already creates and qualifies one WiX MSI.  Its current qualifier proves
  installation, package layout, and one installed NullRHI module-ready marker.
- It does not run the installed module under D3D11/WARP, publish a
  module-scoped smoke artifact, or hand that artifact to `module-evidence`.
- The generic `Tests/PackageSmoke` consumer compiles an SDK client; it does
  not launch the installed `SparkGameFPS.dll`.  Treating its CTest log as
  FPS package proof would be a false claim.
- `SparkGameFPS/CMakeLists.txt` still directly links the in-tree
  `SparkEngineLib` target and adds `SparkEngine/Source` to its include paths.
  Therefore an installed-package runtime smoke cannot truthfully prove the
  public-SDK-only acceptance criterion.

## Alternatives considered

1. Emit the existing generic `SparkInstalledPackageSmoke` CTest output as the
   profile log.  Rejected: it neither loads the real FPS DLL nor exercises the
   stable-v1 package's assets or shipping engine.
2. Add a D3D11 command beside the existing in-job MSI qualification and call
   its diagnostics "evidence."  Rejected: the output would not have a
   deterministic artifact/consumer contract, and the package evidence would
   remain invisible to the Ubuntu release validator.
3. Build the real `MinSizeRel` MSI once, hand it to a dedicated Windows
   package-smoke consumer, then publish its canonical log to the Ubuntu
   module-evidence consumer.  Selected: it makes package identity,
   installation, runtime behavior, and evidence handoff explicit.

## Selected architecture

```
build-windows-shipping (same SHA, MinSizeRel)
  ├─ CPack WiX MSI
  ├─ shipping-package-manifest.json (SHA, version, profile, MSI hash)
  └─ artifact: shipping-package-${SHA}
                  │
                  ▼
module-profile-package-smoke (clean Windows runner)
  ├─ verify manifest + MSI hash + stable-v1 payload
  ├─ install under fresh temporary root
  ├─ run installed SparkGameFPS.dll: NullRHI
  ├─ run installed SparkGameFPS.dll: D3D11/WARP
  ├─ uninstall and prove no residue
  └─ artifact: module-profile-package-smoke-${SHA}
                  │
                  ▼
module-evidence (Ubuntu, rooted/no-follow consumer)
  └─ semantic package-smoke-log validation + exact expected SHA
                  │
                  ▼
required-ci-gate
```

### Shipping package manifest

The shipping producer writes a no-BOM JSON document beside the MSI before
upload.  Its closed schema is `spark-shipping-package-v1` and contains:

- `commitSHA`: exactly `${{ github.sha }}`;
- `profile`: `stable-v1`;
- `configuration`: `MinSizeRel`;
- `version`: the configured semantic engine version;
- `msi`: the expected MSI leaf name; and
- `sha256`: lower-case SHA-256 of that exact MSI.

The artifact also contains the configured `SparkEngineGameModules.cmake` used
to validate the installed package inventory.  The smoke consumer accepts one
regular MSI, one manifest, and one module-inventory file only.  It checks the
manifest SHA/profile/configuration and re-hashes the MSI before invoking
Windows Installer.

### Clean-install smoke contract

`module-profile-package-smoke` runs on a fresh `windows-2022` runner and
depends on `build-windows-shipping`.  It installs the downloaded MSI into a
fresh runner-temp directory, verifies the stable-v1 package layout against the
producer's module inventory, and never uses the source checkout as the
runtime working directory or asset root.

The existing qualification helper becomes the single authority for the
install/uninstall transaction.  It must produce no passing package log until
all of these actions succeed:

1. Windows Installer database identity, clean registration state, and MSI
   digest checks;
2. installed package layout and `SparkGameFPS.dll` sidecar validation;
3. installed `SparkEngine.exe` + `SparkGameFPS.dll` NullRHI run with exact
   module-ready, NullRHI, and headless lifecycle terminal records;
4. installed D3D11/WARP run with the exact direct
   `SPARK_MODULE_LIFECYCLE` terminal record, all required phases positive,
   and `faults=0`; and
5. successful uninstall with no product registration or install-root residue.

The helper writes `build/module-evidence/package-smoke.log` only after steps
1-5 pass.  The log has a closed, line-oriented `package-smoke-v1` contract:
module name, stable-v1 profile, exact commit SHA, MSI SHA-256, both backend
results, `exit_code=0`, and terminal `PASS`.  The semantic validator rejects
missing, duplicate, malformed, mismatched, or non-passing fields.

### Evidence and gate changes

- Add `module-profile-package-smoke` to `module-evidence` dependencies and
  download only `module-profile-package-smoke-${{ github.sha }}` into the
  fixed evidence namespace.
- Update the declared producer job for `package-smoke-log` and remove that
  one entry from `evidence-gaps.json` in the same commit.  Once a producer
  exists, retaining the gap would be a fail-open policy contradiction.
- Add the job to `required-ci-gate`, its exact expected-job JSON, and the
  workflow-failure-propagation test contract.  A skipped producer must fail
  the aggregate gate.
- Keep `module-evidence` on Ubuntu as the only positive rooted release
  authority.  A local or Windows package run is useful production evidence but
  is not hosted release attestation by itself.

## Scope boundary

This slice closes only the RDY-010 `package-smoke-log` gap when an exact-SHA
hosted workflow has passed.  It does not change the truth that MOD-310 remains
open for:

- compiling the full FPS module using only the installed public SDK/exported
  package targets;
- installed single-player spawn/move/kill/respawn/score acceptance;
- save/reload acceptance through the installed package; and
- broader asset/content remediation owned with RDY-020.

CI-120's protected external-attestation requirement also remains unchanged.
Same-workflow artifact provenance is an implementation control, not a
replacement for that independent release certification.

## Verification

Local verification must cover the package-log parser, MSI helper unit tests,
workflow structural/failure propagation tests, exact-required-gate tests,
full module-evidence tests on Windows and WSL/Ubuntu, site-data validation,
and a local `MinSizeRel` lifecycle rehearsal where package tooling is
available.  The final proof is a hosted exact-SHA run through the producer,
Windows package smoke, Ubuntu consumer, and required gate; it cannot be
claimed from local tests alone.
