# Changelog

All notable changes to SparkEngine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Live authored-scene, input, render, HUD, and cleanup loops for every built-in project template and the MultiplayerArena compatibility sample
- Installed-SDK and packaged headless smoke coverage for all nine shipped template projects
- Standalone `SparkServer` dedicated host with dynamic game-module selection, bounded health/stop controls, and gateway-facing area handoff fencing
- `SparkGateway` authenticated ingress and idempotent, epoch-fenced area transfer coordination across owner-local named-pipe and Unix-domain-socket transports
- `SparkOrchestrator` and standalone `SparkCollabServer` processes for daemon-backed lifecycle control and isolated presence, locking, and edit-history traffic
- Deterministic `SparkCooker` and digest-pinned `SparkWorker` asset pipeline, plus `SparkAutomation` black-box runtime, screenshot, log, JSON, and JUnit smoke-test hosting
- Stable, versioned C plugin ABI for importers, processors, editor/runtime extensions, and tools, with deterministic metadata sidecars and a hardened `DynamicPluginHost`
- Editor Dedicated Server and Service Topology panels for configuring, launching, monitoring, draining, and stopping the external service fleet
- Read-only SparkPak inspection commands and unified CLI entry points for cooking, packaging, validation, migration, templates, and package diagnostics
- Provenance-backed hero, wide, and detail runtime galleries for all nine installed-SDK project templates
- Stable rolling Debug/Release installer and ZIP download aliases, lifetime download badges, SPDX release SBOMs, and build-provenance attestations

### Changed
- World saves now write format v2, persist `screenshotPath`, and retain an explicit v1/v2 reader window with an idempotent in-memory v1-to-v2 migration
- Project scaffolding now rewrites `.sparkproject` identity before renaming the descriptor
- Public README, wiki, and badge metrics are generated from the current source and test inventory
- Server, asset-pipeline, automation, SDK, plugin-helper, and public-header targets now install and export with the cross-platform runtime and tools packages
- Editor build/cook packaging now stages complete native dedicated-server packages with rewritten manifests, ABI sidecars, runtime dependencies, and platform launchers before automation runs
- Daemon orchestration persists owner-local client identity, mutation sequences, definitions, desired state, and crash-recovery journals across controller and service restarts
- Every project template now includes validated server/gateway topology configuration; the editor securely provisions each project's owner-only gateway key on first launch
- Runtime packages now include complete deterministic third-party license notices rather than dependency metadata alone

### Fixed
- Failed world restores now leave the live ECS world and caller-owned custom state unchanged for unknown components, deserializer exceptions, and malformed required fields; successful restores retain registry-bound observers and retire stale entity subscriptions
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

## [1.0.0] - 2026-04-04

### Added
- Initial release of SparkEngine
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
