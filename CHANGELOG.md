# Changelog

All notable changes to SparkEngine will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
- CHANGELOG.md, CONTRIBUTING.md, and CODE_OF_CONDUCT.md
- `spark templates` CLI command for listing available project templates
- `spark migrate` CLI command for asset format migration

### Changed
- Updated `spark-cli` with `package`, `validate`, `migrate`, and `templates` subcommands

## [1.0.0] - 2026-03-22

### Added
- Initial release of SparkEngine
- D3D11 primary rendering backend with PBR materials
- Jolt Physics integration (rigid bodies, constraints, vehicles, cloth, ragdoll)
- AI system (behavior trees, NavMesh, perception, formations, steering)
- Animation system (skeletal, blend spaces, IK, retargeting)
- UDP networking with entity replication, prediction, lag compensation
- EnTT-based ECS with 75 component types and 25 systems
- Dear ImGui editor with 57 panels and collaborative editing
- Audio system (XAudio2, OpenAL, null backend)
- AngelScript + Lua scripting with hot-reload
- 10 game modules (FPS, RPG, MMO, ARPG, RTS, Racing, Platformer, OpenWorld, VisualScript)
- 3119 unit tests across 244 test files
- CI pipeline (GCC, Clang, MSVC, ASan, TSan, MSan, coverage, clang-tidy)
