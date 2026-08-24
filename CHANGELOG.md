# Changelog

All notable changes to SparkEngine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Live authored-scene, input, render, HUD, and cleanup loops for every built-in project template and the MultiplayerArena compatibility sample
- Installed-SDK and packaged headless smoke coverage for all nine shipped template projects

### Changed
- Project scaffolding now rewrites `.sparkproject` identity before renaming the descriptor
- Public README, wiki, and badge metrics are generated from the current source and test inventory

### Fixed
- Editor module discovery, project paths, UTF-8 handling, long-path launches, and fail-closed module loading
- Transactional CLI packaging and validated packaged-project launches
- Template module manifests no longer advertise an ignored `loadOrder` field
- Local badge generation now preserves Shields-compatible logos and cache metadata
- World-save readers now classify directories and other non-file paths as unreadable consistently across platforms

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
