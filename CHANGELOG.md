# Changelog

All notable changes to SparkEngine will be documented in this file.

No versioned release has been published. The `stable-v1` profile remains blocked
and uncertified; entries below record source-history milestones, not release
certification.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Production-source regression tests (`Tests/Test*Real.cpp`) covering audio, editor panels/gizmos/undo, engine wiring, save round trips, security parsers, the shader compiler, the shadow pass, module lifecycle, user data paths, logger sinks, crash-handler gating, and the SparkGameFPS loop; `Tools/test_source_census.py` reports production-source vs mirror tests with a `--check` gate
- Test runner: `EXPECT_WARN_ONLY(expr, reason)`, `EXPECT_NO_CRASH(reason)`, an `[ EMPTY ]` label for zero-assertion tests (`--empty-is-error`), and JUnit `flaky=`/`empty=` attributes with `<flakyFailure>` instead of `<skipped>` for waived tests
- `SparkEngineLoadTests` CTest lane (labels `load;slow`) split from `SparkEngineTests`, with per-configuration `SPARK_TEST_TIMEOUT_SECONDS` budgets and the `SPARK_TESTS_WARN_IS_ERROR` option
- SparkGameFPS: complete death -> respawn -> score loop (`RespawnSystem` decoupled from `Player`), `FPSLocalProfile` persisted by `quicksave`/`quickload`, one runtime-discovered asset root, and initialization without a D3D11 device
- Save format v3: Transform hierarchy persisted and restored, `<slot>.spark_save.bak` last-good retention with load fallback, `SaveMetadata::slotName` on enumerated slots, on-demand save-directory creation
- `Spark::UserPaths`: per-user `Saves/`, `Logs/`, `spark_trace.json`, `ShaderCache/`, and `settings.ini` fallback under `%LOCALAPPDATA%/SparkEngine` (XDG directories on POSIX)
- Engine logs persisted to `Logs/SparkEngine_<timestamp>.log` via `Logger::InstallDefaultSinks` and mirrored to `SparkConsole.exe`; console commands carry an owner token so a second registrant cannot replace a command or lower its permission level
- D3D11 shadow-caster depth pass (Deferred and RenderGraph paths) with `LightingSystem` driving the cached shadow atlas; real triangle/vertex counts in render statistics
- `Json::ParseBounded` / `JsonLimits`, `Spark::IsVirtualPathSafe`, `PacketValidator::ValidateStringFields` with per-schema `stringFieldOffset`, per-entry `.spk` decompression budget (256 MB ceiling, 100000:1 factor, enforced in `ReadFile` so one bad entry does not unmount the archive), scene-manifest size/entry caps, `.skel`/`.sanim` name-length and bone-index validation
- `IEngineContext::GetInvalidStateDetector()` / `GetComponentSerializers()` host instances; `GetLocalization`/`GetAI`/`GetAnimation`/`GetWeapons`/`GetMusic`/`GetDestruction`/`GetAbilities`/`GetConditions`/`GetInstances`/`GetTween`/`GetVFS`/`GetAreaStreaming` populated by the gameplay lifecycle
- `TriggerVolumeComponent` bridged to `ProximityTriggerSystem` (enter/exit publish `TriggerEnterEvent`/`TriggerExitEvent`); `ReplaySystem` records Transform/velocity/health frames while recording
- Editor: SparkEditor installs a real Windows unhandled-exception filter (dumps under the per-user editor data directory); `EditorWindowManager` layout persisted to `<EditorData>/window_layout.json`; Workflow panel confirmation for Clean & Rebuild
- CI/readiness tooling: `asset-integrity` job (`tools/site-data/validate.py --assets`), strict work-item selector/job resolution with `plannedCiJobs`/`plannedTestSelectors`, docs health producer (`docs/.health.json`), `SparkVersionSingleSource` CTest, `SPARK_CRASH_ON_ASSERT=1`, sanitizer `incomplete-run` classification, `.github/test-count-ratchet.json` baseline block
- `CrashHandler::TriggerCrashReportUnattended()` (`CrashReportDelivery::ArtifactOnly`): dump/log/manifest with no screenshot, consent dialog, or in-process upload — used by the freeze watchdog so `terminateOnFreeze` really terminates
- `SaveSystem::MarkComponentTransient()` / `IsComponentTransient()` opt-out for registry-driven world serialization (default transient set: `ProjectileComponent`, `DecalComponent`)
- Console command `log_path`: prints the log file this run opened and its size, or reports that no log file could be opened
- Live authored-scene, input, render, HUD, and cleanup loops for every built-in project template and the MultiplayerArena compatibility sample
- Installed-SDK and packaged headless smoke coverage for all nine in-tree template projects
- Standalone `SparkServer` dedicated host with dynamic game-module selection, bounded health/stop controls, and gateway-facing area handoff fencing
- `SparkGateway` authenticated ingress and idempotent, epoch-fenced area transfer coordination across owner-local named-pipe and Unix-domain-socket transports
- `SparkOrchestrator` and standalone `SparkCollabServer` processes for daemon-backed lifecycle control and isolated presence, locking, and edit-history traffic
- Deterministic `SparkCooker` and digest-pinned `SparkWorker` asset pipeline, plus `SparkAutomation` black-box runtime, screenshot, log, JSON, and JUnit smoke-test hosting
- Versioned C plugin ABI for ongoing compatibility work (not `stable-v1` certification) across importers, processors, editor/runtime extensions, and tools, with deterministic metadata sidecars and a hardened `DynamicPluginHost`
- Editor Dedicated Server and Service Topology panels for configuring, launching, monitoring, draining, and stopping the external service fleet
- Read-only SparkPak inspection commands and unified CLI entry points for cooking, packaging, validation, migration, templates, and package diagnostics
- Provenance-backed hero, wide, and detail runtime galleries for all nine installed-SDK project templates
- Rolling Debug/Release build aliases and generated checksum/SBOM/provenance metadata for development artifacts; binaries/installers are not code-signed, and none of this is versioned stable-v1 release qualification

### Changed
- World saves now write format v3 (reader window v1..v3, in-memory v1->v2 and v2->v3 migrations); `SerializeWorld` covers every `ComponentFactory`-registered type with a serializer instead of a fixed 14, and named entities without other components are retained
- `RHIBridge::Initialize` no longer silently degrades a windowed request to `NullRHIDevice`; headless fallback requires `allowHeadlessFallback`. D3D11 requires feature level 11_0 (SM 5.0); D3D11 deferred command lists really record (`FinishCommandList`) and execute; structured/indirect buffers get the correct misc flags and SRV/UAVs
- Shader compiler compiles HLSL to DXBC for real via `d3dcompiler_47` (D3D11/D3D12 on Windows) and fails closed for DXIL/SPIR-V/GLSL/MSL; shader hot reload compiles for real when driven but is not enabled in production; the four DXR PSOs use their shaders' export names
- Basic shaders rewritten: the embedded sources in `GraphicsDeviceResourcesWindowsShaders.cpp` are canonical, `Shaders/HLSL/BasicVS.hlsl`/`BasicPS.hlsl` are the matching on-disk copies, no prebuilt `Basic*.cso` is shipped, and the shader tree is no longer flattened on install
- Packet string screening is now enforced: schemas declare `stringFieldOffset` (or `NO_STRING_FIELDS`) instead of a `sanitizeStrings` flag; `ChatMessage` is one NetBuffer string at offset 0; the `packet.stats` `string:` counter is a real count; UTF-8 text is accepted (signed-char defect fixed)
- `Json::Parse`/`ParseStrict` enforce the default `JsonLimits` budget (32 MB, depth 128, 4M nodes); `mod.json` and mod configs are capped at 64 KB, depth 16, 4096 nodes and parsed strictly; scene manifests are capped at 8 MB / 100,000 entries and drop escaping, absolute, or device-named asset paths
- `Json::ParseStrict`/`ParseBounded` reject unpaired `\u` surrogate escapes that previously decoded to WTF-8, so a file that used to parse can now fail; the lenient `Parse()` returns `Null` with only a warning when the default budget is exceeded, which a caller treating `Null` as "empty document" reads as an empty success
- MMO chat registers its **own** `MessageType` (`UserDefined + 1`, `stringFieldOffset = 1`) instead of re-registering the shared built-in `ChatMessage` schema process-wide; the built-in `ChatMessage` schema is untouched
- `.skel`/`.sanim` loaders abort the whole file on any corruption (rejected length prefix, out-of-range count, non-finite matrix, invalid parent index, truncation inside a channel): no partial parse, no "Loaded N" line, and a failed skeleton load is not cached. `SceneManifest::ParseFromFile` fails closed when `file_size` reports an error, and entry-cap truncation is logged rather than silent
- Virtual-path policy: a name merely containing `..` is legal, and a zero-byte override file wins the mount-priority contest; `ModSystem` fails closed on symlinked mod directories
- Audio: `AudioEngine` registers an `IXAudio2EngineCallback` and detects device loss for already-playing sources through `OnCriticalError` (consumed on the game thread in `Update`), not only from a failing `PlaySound`; `RecoverDevice` recreates submix voices against the new device, so any `IXAudio2SubmixVoice` pointer held across a recovery is invalidated and must be re-fetched. Four `AudioEngine` tests now report `[ SKIP ]` on a host with no output device where they previously reported `[ OK ]`, changing the expected pass total on device-less runners
- Audio: mix-bus volume/mute/solo and occlusion (when a `PhysicsSystem` is attached) are applied to live sources; master volume is applied once on the mastering voice; XAudio2 device loss (`XAUDIO2_E_DEVICE_INVALIDATED`) rebuilds the mastering voice; the 3D listener follows the engine camera; `AudioSourceComponent` with `playOnAwake` binds once
- Editor panels that displayed fabricated data now read real backends (Scene Statistics, Search, Save System, Replay, Modding, Time of Day, Weather & Fog); panels with no backend (Debug Visualizer, Object Placement modes, Dedicated Server discovery) show `Preview - not connected`; the Game View HUD is labelled simulated and no longer writes the authored camera; Weapon Editor's no-op "Save All" and Prefab Editor's inert "Copy Component" were removed
- Editor gizmos: translate, rotate, and scale all apply to World entities with one `CommandHistory` entry per drag; toolbar, `W`/`E`/`R`, command palette, and Scene View toolbar drive one `EditorUI::SetTransformTool`; World-mode rename (`F2`), `Ctrl+D` duplicate, and `Delete` are command-backed
- Editor build workflows configure with the host preset (`windows-release`) instead of `linux-gcc-release`; Clean & Rebuild only deletes the open project's build directory
- Crash reporting is one report per process: `HandleCrashInternal` carries a once-guard, so an assertion reaching both `TriggerCrashHandler` and `TriggerCrashReport` writes a single dump/log/manifest and any later trigger is ignored. Redaction rules fall back to `SHGetFolderPathW`/`GetUserNameW`/`GetComputerNameW` when the environment supplies none, and an upload is refused (report kept local) when no rules can be derived. `SymStackTrace()` and `ThreadStacks()` use a 500 ms bounded try-lock on the DbgHelp symbol lock, emitting unsymbolized addresses rather than blocking
- `Logger::InstallDefaultSinks` warns through the sinks it just installed when a file sink was requested but no log file could be opened, naming the directory; an empty returned path no longer conflates "not requested" with "could not be created". It calls `ClearSinks()`, so engine sinks must be installed before editor panels register theirs. The engine->SparkConsole mirror queue is bounded at 4096 lines with drop-oldest and one dropped-line notice per burst; `ConsoleProcessManager::Initialize()` returning true means "initialization attempted without error", not "console running" (`IsConsoleRunning()` is the liveness answer)
- Crash reports uploaded off the machine are redacted (profile paths -> `%USERPROFILE%`/`%LOCALAPPDATA%`/`%TEMP%`, machine name masked before the account name it contains); uploads bounded at 30 s per request with a stall cut-off and a 32 MB attachment cap; repeated crashes deduplicate onto one issue; `FreezeDetector::Start()` is a no-op under `SPARK_SHIPPING`
- Working-directory anchoring: only a **relative** `-game`/`-manifest`/`-scene` suppresses `AnchorWorkingDirectory`. An absolute one now re-anchors the working directory to the executable directory, which is what lets a packaged runtime find `Data/*.spk` and keep `Logs/`/`Saves/` together — tooling that passes an absolute module path and relies on other relative paths resolving against its own CWD needs checking
- Weather, TimeOfDay, Dialogue, UI, SeamlessArea, Tween, Cinematic and Replay playback moved off `JobSystem` batches onto serial main-thread calls (none is thread-safe); `UpdateProximityTriggers` and `CaptureReplayFrame` are new per-frame main-thread producers using persistent scratch buffers. The frame-time budget needs re-measuring
- `SPARK_SDK_VERSION` is 4 after `IEngineContext` gained `GetInvalidStateDetector()`/`GetComponentSerializers()`; `IsSDKCompatible` is exact equality, and `Spark/IEngineContext.h` pins `EngineContextVirtualCount = 90` with a `static_assert` tying the two together
- Editor: `PrefabManager::ApplyPrefabToInstances` writes a recorded instance override's stored value onto the scene object instead of treating the override only as a skip-list key; `SearchPanel` asset results come from a real cached recursive scan of `<project>/Assets` (debounced, capped at 50) and entity/component results require a wired `World`; `ProjectManager` refuses to write any `.sparkproject` under the engine's `Templates` root; Build and Clean & Rebuild fail outright when no project is open, and Clean returns failure on a removal error. Version control still rejects any user-supplied URL, path, remote, or branch beginning with `-` (git option injection)
- Windows startup flags are parsed as exact tokens (`-game <path containing -headless>` no longer switches to headless; `-threadsafe` no longer matches `-threads`); `spark.modules.json` resolves `lib<Name>.so`/`.dylib` from a `.dll` path on POSIX; failed module `OnLoad` no longer blocks a replacement game module; engine teardown runs module `OnUnload` before gameplay/debug shutdown
- Ten never-fed lifecycle registrations removed (see Removed); `EngineContext::InitializeAll` is documented as not the production init path
- `STRIP_DEBUG_SYMBOLS` is real (suppresses MSVC `/DEBUG`, adds `-s` on GNU/Clang); `ENABLE_LTO` drives MSVC `/GL` + `/LTCG`; NSIS/WIX generators are appended only when found (`SPARK_REQUIRE_WINDOWS_INSTALLERS=ON` fails configure otherwise); `build.ps1` drops the non-existent `-console`/`ENABLE_CONSOLE` option and passes `--parallel 1` for Visual Studio generators
- `.sparkterrain` is version 2 (explicit version word, `generateCollider`, per-layer texture paths and material parameters, serialized detail meshes); version 1 files are rejected. Editor terrain saves default to `Assets/Terrains/<name>.sparkterrain` with overwrite confirmation
- Editor version-control panel launches git from an argument vector with no shell; commit messages, branch names, and paths with spaces, quotes, ampersands, or semicolons are accepted verbatim
- FSR2/DLSS/XeSS report unavailable while no vendor SDK is linked (SparkSR substitutes with one logged warning); per-frame renderer console INFO spam removed
- Templates: RPGStarter saves persist to `Saves/rpg_slot0.spark_save`; new projects no longer inherit a template's `dist`/`build`/`Logs`/`Saves`; project creation rewrites the package token in `.ini` service configs; the editor refuses to write inside the template package root; single-player templates ship `score_limit`/`time_limit_minutes` 0; MultiplayerArena's `server.ini` matches its module; `EmptyProject` ships an empty authoring scene plus an 8-entity runtime preview
- Public metric label for the harness is "SparkTests TEST and TEST_F definitions"; `code.totalLines`/`code.files` use the tracked first-party corpus shared with the badges; docs health status is `current | refresh-pending | skipped` (never `unknown`) and publication is refused unless `current`
- Site-data validator resolves every work-item `requiredCiJobs`/`testSelectors` entry against real workflow jobs and CTest/SparkTests names; unresolvable entries must be declared in `plannedCiJobs`/`plannedTestSelectors`
- World saves persist `screenshotPath` and previously (format v2) retained an explicit v1/v2 reader window with an idempotent in-memory v1-to-v2 migration
- Project scaffolding now rewrites `.sparkproject` identity before renaming the descriptor
- Public README, wiki, and badge metrics are generated from the current source and test inventory
- Server, asset-pipeline, automation, SDK, plugin-helper, and public-header targets now install and export with the cross-platform runtime and tools packages
- Editor build/cook packaging now stages complete native dedicated-server packages with rewritten manifests, ABI sidecars, runtime dependencies, and platform launchers before automation runs
- Daemon orchestration persists owner-local client identity, mutation sequences, definitions, desired state, and crash-recovery journals across controller and service restarts
- Every project template now includes validated server/gateway topology configuration; the editor securely provisions each project's owner-only gateway key on first launch
- Runtime packages now include complete deterministic third-party license notices rather than dependency metadata alone

### Fixed
- `AssetCache::AddAsset` re-locked its own non-recursive mutex through the public `GetCurrentMemory()`/`EvictLRU()` while enforcing the budget; on MSVC the re-lock throws `system_error(resource_deadlock_would_occur)`, so every synchronous `AssetPipeline::LoadAsset` that reached the cache threw. Budget enforcement now runs on lock-held helpers, `EvictLRU()` locks for external callers, and the loop terminates when a single asset exceeds the whole budget (`Tests/TestAssetPipelineReal.cpp` RED proofs; surfaced by the shadow-pass test loading three meshes)
- `InstallEngineLogSinksImpl` trusted its once-per-process latch even after something had called `Logger::ClearSinks()` or `Logger::Shutdown()` behind it, returning a log path nothing was writing to; it now re-checks the Logger (`GetSinkCount()`, `IsInitialized()`), reinstalls when torn down, and initializes the Logger itself when no entry point has
- SparkGameFPS: the player is revived on respawn instead of leaving an inert corpse; `quicksave`/`quickload` report the real result instead of an unconditional "Save system ready."; the duplicate Sequencer/Replay tick that ran cutscenes and replay playback at 2x was removed; `InvalidStateDetector` rules query the module's own Player/Enemy state
- Audio: master volume was applied twice; SFX volume was ignored by live sources; `Console_RefreshAudio` compounded gain; the XAudio2 device-loss fallback HRESULT was `XAUDIO2_E_INVALID_CALL` (0x88960001) instead of `XAUDIO2_E_DEVICE_INVALIDATED` (0x88960004)
- Crash redaction masked the account name before the host name that contains it, leaving the host suffix in uploaded reports; a watchdog-detected freeze and a fatal assertion now leave a dump/manifest on disk; the stack hash parses the engine's own frame format
- `SaveSystemPanel::CanLoad()` no longer advertises a load with an empty slot list; `SetSaveDirectory` now really creates the directory on the next save; `AsyncDatabase`'s key-value store no longer truncates the live file on every write; `QuestSystem` completion grants item rewards and publishes `QuestCompletedEvent`
- `SparkConsole.exe` no longer feeds its own banner to the engine as commands; stdout is reserved for engine-bound commands on Windows as well as Linux; `SPARK_LOG_*` output reaches the console process
- D3D11 deferred command lists were a dangling context with a no-op submit; `RHIBridge` silently fell back to NullRHI for windowed requests; the shader compiler reported success with empty bytecode for every non-DXBC target; `ShaderHotReload` de-duplicates canonicalized watch directories and bounds its scan
- Skeleton/animation loaders desynced on an out-of-range name-length prefix and still reported success; `AssetCooker::IsContained` normalizes both sides and rejects `..` anywhere; `ReadDirtyMask` returns `[[nodiscard]] bool`
- `SceneStatisticsPanel`, `SearchPanel`, and `ObjectPlacementPanel` no longer render sample data; the Console panel's non-existent regex/json export claims were removed from the UI
- `check-cross-utilization.sh`, `check-di-singletons.sh`, and `check-wiring.sh` no longer report success when ripgrep is missing (they fall back to grep and exit 2 with neither); `check-test-registration.sh` inventories `*Probe.cpp` and reports conditional registrations; `check_network_boundary.py` judges the git-tracked tree (`SparkNetworkBoundaryStatic` passes on a dirty working tree)
- Sanitizer classification distinguishes a suite that died before writing JUnit from a clean run; the exact-source CI gate rejects a required job marked `continue-on-error`
- Pre-commit world-restore failures now leave live ECS/entity-subscription and caller-owned custom state unchanged for unknown or duplicate components, deserializer exceptions, transient candidate entities, and malformed required fields; successful restores use pristine entity-allocation state, retain and explicitly rebind registry-bound reactive observers, while lifecycle-observer exceptions propagate as fatal programming faults because observer-owned side effects are not rollback-capable
- Editor module discovery, project paths, UTF-8 handling, long-path launches, and fail-closed module loading
- Transactional CLI packaging and validated packaged-project launches
- Template module manifests no longer advertise an ignored `loadOrder` field
- Local badge generation now preserves Shields-compatible logos and cache metadata
- World-save readers now classify directories and other non-file paths as unreadable consistently across platforms
- Plugin load and hot-reload now verify binary identity before and after mapping, quiesce scheduled work before unload, reject unsafe package paths, and preserve the working image on staged-load failure
- Asset cooking now hashes the exact staged bytes, rejects manifest escapes and linked outputs, and atomically replaces cooked artifacts and manifests
- External-process launch, cancellation, timeout, stderr capture, process-tree teardown, gateway partial-frame handling, endpoint ownership, and crash-state persistence are bounded and fail closed across Windows, Linux, and macOS
- Windows external-process launch now preserves UTF-8 project paths through `CreateProcessW`; installed launcher templates outrank caller working directories, and failed editor-plugin rollback retains truthful fail-closed ownership
- POSIX orchestration now durably records child identity before releasing `exec`, closing the daemon-crash orphan window

### Removed
- Lifecycle registrations for never-fed systems: `PipelineStateCache` (RHI), `OcclusionCullingSystem`, `LightProbeSystem`, `TransientResourcePool`, `VirtualTextureManager`, `DynamicQualityScaler`, `GPUStallProfiler`, `AsyncComputeScheduler`, `AIDebugRenderer`, `HLODSystem`, and the duplicate `Engine/HotReload/ModuleHotReload` singleton (the live module hot reload is `Spark::ModuleHotReloadManager`); the two `FileLogger` lifecycle calls (engine logs come from `Spark::FileSink`)
- Editor code with no production caller: `Gizmos/GizmoSystem` (duplicate of the Scene View gizmos), `Panels/PostProcessingPanel` (use the Inspector's Post-Process Volume component), the standalone `MaterialEditor/MaterialEditor`, `Lighting/LightingTools` (its "lightmap bake" wrote a fixed radial ramp and discarded its light probes), the `Animation/` timeline classes, and `AssetBrowser/AssetDatabase`; decoy tests `Tests/TestAssetDatabase.cpp` and `Tests/TestPrefabManager.cpp`
- SparkGameFPS in-game console overlay (`Game/Console`), `Game/Terrain`, and the commented-out `Game/ArenaBuilder`
- Prebuilt `Shaders/Compiled/BasicVS.cso` / `BasicPS.cso` (both copies); `MessageSchema::sanitizeStrings` (replaced by `stringFieldOffset`); the never-instantiated `GraphicsEngine::m_shader` member; `ProfileScope`/`PROFILE_SCOPE`; `SaveSystemPanel` Export/Import; dead Windows `m_rhiBridge`/HybridRT branches (`GraphicsEngine::GetRHIBridge()`/`GetRHIDevice()` return `nullptr` on Windows)
- Documentation claims that the code does not implement: `/W4` zero-warning builds, in-game backtick console, LAN server discovery/RCON/kick-ban in the Dedicated Server panel, atlas-backed shadow sampling, automatic DLSS/XeSS/FSR2 selection, per-bus DSP and rendered reverb, hybrid RT on D3D11, an enforced merge-time Required CI Gate

### Security
- Packet text fields are screened for real (schema-declared offsets, length-prefix validation) and the `packet.stats` string counter is no longer a permanent zero
- JSON parse budgets on every default entry point; strict, size-capped `mod.json`/mod-config parsing; scene-manifest path containment and size caps; `.spk` decompression-ratio bound enforced at `Open`; skeleton/animation loaders reject non-finite matrices and forward `parentIndex` references
- Virtual-path policy rejects drive-relative and NTFS alternate-data-stream names, reserved Windows device names, and symlink/junction escapes inside a mount
- Crash reports uploaded off the machine are redacted (paths, account, and machine names) while the local copy keeps full paths; uploads are time- and size-bounded
- Console command registration carries an owner token: a second registrant cannot silently replace a command or downgrade its permission level
- Editor git operations launch from an argument vector with no shell (no command-line injection through paths or messages)
- Dedicated `SparkEditor` crash filter writes dumps under the per-user editor data directory instead of the working directory

## Historical implementation baseline — 2026-04-04 (unreleased)

### Added
- Initial source baseline of SparkEngine; not a published 1.0.0 release
- D3D11 primary rendering backend with PBR materials
- Jolt Physics integration (rigid bodies, constraints, vehicles, cloth, ragdoll)
- AI system (behavior trees, NavMesh, perception, formations, steering)
- Animation system (skeletal, blend spaces, IK, retargeting)
- UDP networking with entity replication, prediction, lag compensation
- EnTT-based ECS with 75 component types and 25 systems
- Dear ImGui editor with 59 panels and collaborative editing
- Audio system (XAudio2, OpenAL, null backend)
- AngelScript + Lua scripting with hot-reload
- 10 game modules (FPS, RPG, MMO, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript)
- 3534 unit tests across 281 test files
- CI pipeline (GCC, Clang, MSVC, ASan, TSan, MSan, coverage, clang-tidy)
- Game packaging pipeline (`spark package` CLI command) for creating distributable builds
- Asset validation system (`spark validate` CLI command) for content integrity checking
- Asset format versioning and migration system with `AssetFileHeader` (SPRK magic)
- Accessibility framework: colorblind filters, subtitles, high-contrast mode, reduced motion
- Cross-platform input abstraction layer (`PlatformInput.h`) with SDL2 backend support
- Performance regression benchmark framework (`BenchmarkFramework.h`)
- 4 new project templates: FPSStarter, RPGStarter, PlatformerKit, MultiplayerArena
- Declarative UI layout extensions: FlexContainer, GridLayout, ScrollView, TextInput, Slider, Dropdown
- Runtime telemetry and analytics system with privacy-consent API
- Achievement system with progress tracking, tiers, and save integration
- Shader hot-reload system with file watching and automatic recompilation
- Runtime prefab system for spawning entities from prefab definitions at runtime
- Interactive editor tutorial system with step-by-step guided workflows
- Golden image / screenshot regression testing framework
- API changelog generation tool (`tools/api-changelog.py`)
- CHANGELOG.md, CONTRIBUTING.md, CODE_OF_CONDUCT.md, and SECURITY.md
- `spark templates` CLI command for listing available project templates
- `spark migrate` CLI command for asset format migration
- `spark-cli` with `package`, `validate`, `migrate`, and `templates` subcommands
