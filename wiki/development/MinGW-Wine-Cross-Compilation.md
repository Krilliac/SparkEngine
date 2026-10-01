# MinGW + Wine CPU Rendering (experimental)

> **Audience:** Programmers and automation agents | Mixed
>
> **Thread Context:** Build host; engine/editor commands execute on their main threads.
>
> **Platform/Backend Scope:** Linux/WSL host, Windows x64 D3D11 binaries, Wine + DXVK + Lavapipe.

## Capability and proof boundary

This experimental path has an advisory, manual `workflow_dispatch` CI lane.
OD-30 (owner, 2026-10-01) keeps CPU rendering for GPU-less servers and AI agents
as a claimed capability for **both the engine and SparkEditor**. It is not a
certified Windows release row. D3D12 remains excluded from the MinGW build.

The last supplied hosted evidence (2026-09-15, run 34983218822, job 104483285541)
configured and built successfully, then failed during Wine tests. The owner
reports the same build-success/test-failure result in four dispatch runs.
Those runs do not prove the new engine/editor smokes. Current local Wine
execution and an exact-commit hosted run remain pending; CI-100[2] is `unmet`.
The 2026-10-01 implementation session could not run WSL, Wine or a C++ build.
Historical March timing and test counts are not current acceptance evidence.

## Stack and prerequisites

Windows D3D11 engine/editor -> MinGW-w64 -> Wine -> DXVK 2.5.3 -> Mesa Lavapipe
(CPU Vulkan). WineD3D + llvmpipe remains a development fallback.

NullRHI rasterizes nothing. Native Linux fallback does not exercise Windows.
Neither diagnostic fallback proves this capability.

Use an existing GCC 13+ MinGW-w64 toolchain and CMake 3.25+. On a host with
passwordless sudo, missing dependencies may be installed through signed APT:

```bash
sudo -n true
sudo -n apt-get update
sudo -n apt-get install -y mingw-w64 cmake wine64 wine mesa-vulkan-drivers xvfb xauth curl ca-certificates
```

If passwordless sudo is unavailable and dependencies are missing, ask the owner
to install them; do not download unverified `.deb` files or bypass APT checks.
This lane's supplied WSL environment had MinGW/Lavapipe/Xvfb but no Wine.

The toolchain owns DirectXMath: it verifies the oct2024 archive using
`cmake/toolchains/SparkVerifiedDirectXMath.cmake`. Do not fetch headers from
`main` or install a second unpinned copy into the MinGW sysroot.
`tools/setup-mingw-wine.sh --dxvk-only` pins the exact DXVK release URL and
SHA-256 before extraction, including repeated setup. The digest is corroborated
by [Winetricks tag 20260125](https://github.com/Winetricks/winetricks/blob/20260125/src/winetricks#L7302).
A hash mismatch fails without installing the archive. An explicitly supplied,
initialized `WINEPREFIX` also receives the verified D3D11/DXGI DLLs.

## Reproduce the acceptance path

The preset disables Vulkan/OpenGL/SDL2 engine backends and uses Windows D3D11.
The commands below build both applications and all test/module fixtures.
Serialize with other C++ builds and respect the host memory preflight.

```bash
# From the Linux/WSL checkout root; initialize the recorded submodules first.
git submodule update --init --recursive
cmake --preset linux-mingw-release -DBUILD_TESTS=ON -DENABLE_EDITOR=ON
cmake --build build/linux-mingw-release --target SparkEngine SparkEditor --parallel 2
cmake --build build/linux-mingw-release --parallel 2

# Use a dedicated prefix. --dxvk-only verifies the archive and copies its x64 DLLs here.
export WINEPREFIX="$PWD/build/linux-mingw-release/.wineprefix-mingw"
xvfb-run -a bash tools/wine-run.sh --setup-only
bash tools/setup-mingw-wine.sh --dxvk-only

# Each command invokes xvfb-run + Wine + pinned DXVK + Lavapipe internally.
python3 .github/scripts/mingw-wine-smoke.py engine
python3 .github/scripts/mingw-wine-smoke.py editor
export SPARK_TEST_EXCLUDE="$(python3 .github/scripts/mingw-wine-smoke.py exclusions)"
python3 .github/scripts/mingw-wine-smoke.py tests
python3 .github/scripts/mingw-wine-smoke.py summary
```

The runner writes fresh per-process evidence under `build/mingw-wine-evidence/`.
Use `--build-dir` and `--output` to select other owned build/evidence directories.
Local summary build stages say `not-run` unless their outcomes were supplied;
they are not inferred from old binaries. The CI job supplies actual step outcomes.

## What the smokes require

- **Engine:** the checked-in editor-authored scene loads (3 entities, 1 renderable),
  `-exec` disables VSync and issues captures at frames 5 and 55, `-exec-audit`
  confirms command execution and positive draw/triangle counts at frame 59,
  both fresh PNGs decode at the window client size (320-640 by 240-480) and contain more than the clear color,
  and the process exits 0 after 60 loop frames. Capture hashes are recorded.
- **Editor:** CLI automation creates a project, saves the seeded scene through
  `--save-scene`, reopens it through `--open-scene`, and runs 60 frames. The
  `--smoke-result` JSON requires a loaded project, `d3d11`, 60 successful
  `Present == S_OK` calls, zero presentation failures and exit 0. Occluded
  presentation statuses do not count. This uses existing CLI commands, not a
  claim that the separate EditorAutomation registry has a CLI bridge.
- **CPU path:** both smokes require fresh DXVK 2.5.3 D3D11 logs naming a
  llvmpipe/Lavapipe device. Both Vulkan loader variables point only to the
  Lavapipe ICD; hardware fallback cannot satisfy the checks.
- **Tests:** the runner uses the exact named exclusions below, removes inherited
  test selectors, passes `--warn-is-error`, requires exit 0 and a terminal
  summary with the passing-test floor (`MINIMUM_TESTS=7500`) and no failures/warnings. Skips do
  not count toward the floor. The floor is a minimum, not a discovered total.

`tools/wine-run.sh` normally auto-adds `-headless` and `-minimal-init` to the
engine. This acceptance runner disables those auto-flags. The legacy
`tools/test-windows-wine.py` accepts some rc=255 results and remains a diagnostic
utility; it is not the acceptance producer. The new runner rejects rc=255.

## Known failures and Wine-only exclusions

The canonical machine-readable list is `.github/scripts/mingw-wine-exclusions.json`.
`SPARK_TEST_EXCLUDE` is set only in the Wine job/command. Native Windows tests
remain unchanged. Credential signature, parsing and replay tests are not
excluded wholesale. Classification uses the supplied hosted failures plus the
current source contracts; a new failing case requires diagnosis, not a wider
substring exclusion.

| Test excluded only under Wine | Reason |
|---|---|
| `VersionControlPhaseAA_InitializeShutdown` | Windows Git is absent in the Wine prefix |
| `GPUDriven_D3D11_ShaderReflectionMatchesSharedABI` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `GPUDriven_D3D11_PrimitiveLosesSparseSourceIdentityAndProductionGateFailsClosed` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `PostProcessingD3D11_ZeroAndOnePassRouteWithoutViewHazards` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `WorldBasicRender_DrawsGeometryIntoOffscreenRTV` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `WARP_D3D11DeviceInit` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `WARP_D3D11BufferCreation` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `WARP_D3D11FactoryCreate` | Native WARP and D3DCompiler include/reflection semantics; retained on real Windows |
| `StackTrace_FramesHaveAddresses` | Wine stack walking does not supply native Windows frame addresses |
| `GatewaySecurity_AcceptsOwnerOnlyGeneratedKeyFile` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewaySecurity_FailedReloadRevokesPriorOutputKey` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewaySecurity_RejectsHardLinkedKeyFile` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewaySecurity_GeneratedKeyRemainsPrivateUnderInheritableParentAcl` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewaySecurity_RejectsUnprotectedAcl` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewaySecurity_AllowsConcurrentOwnerReadHandle` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewaySecurity_RejectsOwnerOnlyAclWhenOwnerIsNotCurrentProcessUser` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `CrashHandler_UngatedReportWritesAnArtifactAndTheAssertGateDoesNot` | Wine does not implement the owner-only protected DACL, owner and hard-link security contract |
| `GatewayAreaControl_LiveLoopbackIsIdempotentAndPersistsEpochFence` | Wine named-pipe disconnect and PeekNamedPipe timing differ from native Windows |
| `GatewayAreaControl_RecoversWhenClientDisconnectsBeforeAccept` | Wine named-pipe disconnect and PeekNamedPipe timing differ from native Windows |
| `UserDataPaths_ResolveSaveDirectoryMigratesLegacyWorkingDirectorySaves` | Wine Windows profile-directory mapping differs in the legacy-save migration fixture; native recheck required |
| `ModuleHotReload_FailedPollKeepsChangePendingForRetry` | Wine mapped-PE replacement/reload differs in the real DLL fixture; native retry/exception tests stay enabled |
| `ModuleHotReload_PollChangesContainsNonStandardCallbackExceptions` | Wine mapped-PE replacement/reload differs in the real DLL fixture; native retry/exception tests stay enabled |
| `AudioEngineReal_PooledVoiceIsRebuiltForANewSoundFormat` | Hosted runner has no ALSA device; Wine FAudio device enumeration crashed in run 34983218822 |

The ACL classification follows `CrashArtifactDirectory.h` and the gateway's
protected-DACL/owner/hard-link checks. Named-pipe classification follows the
`GatewayAreaControl.cpp` comment on Wine `PeekNamedPipe` peer-closure behavior.
The profile-path and mapped-PE fixture classifications are provisional platform
limitations: no current Wine reproducer was available to establish a new engine
bug. Re-run their native counterparts and re-evaluate these exclusions when
Wine changes. Never relax the Windows security requirements to satisfy Wine.
The FAudio crash was an access violation during device enumeration with no ALSA
card in the supplied hosted log; excluding that case does not establish audio support.

## CI and readiness

`build-linux-mingw-wine (experimental)` stays `workflow_dispatch` only,
job-level `continue-on-error: true`, and outside `required-ci-gate` and its
expected-job inventory. There is no nightly schedule: three consecutive local
passes have not been demonstrated.

Separate steps configure, build the engine, build the editor, build remaining
targets, run the engine smoke, run the editor smoke, and run Wine tests. An
`always()` step emits `summary.json`, `summary.md` and the GitHub step summary,
including unsuccessful/skipped build stages and runtime evidence. An `always()`
artifact uploads receipts, captures, execution audits and raw logs even after a
failure. Only the compiler cache is restored; prior build receipts are not cached.

CI-100[2] may become `implemented` only after local CPU-render engine and editor
runs succeed. An integrator-owned exact-commit hosted dispatch is the separate
`evidenced` step. This implementation does not dispatch workflows.

## Source & Freshness

Updated 2026-10-01 from the assigned owner context, the existing CLI/graphics
source, workflow and contract checks. Runtime proof remains pending.

## Related Pages

- [Cross-Compilation: Wine Testing](../platform/Cross-Compilation-Wine-Testing.md)
- [CI Reproducible Builds](CI-Reproducible-Builds.md)
- [Testing](../advanced/Testing.md)
- [Wine Role and Fallback Tiers](../advanced/Wine-Role-and-Fallback-Tiers.md)
