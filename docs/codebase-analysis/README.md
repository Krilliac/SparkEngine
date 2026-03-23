# SparkEngine — Comprehensive Codebase Analysis

**Generated:** 2026-03-23
**Engine Version:** 1.0.0 (SDK v2)
**Language:** C++23 (with C++26 forward-compatibility macros)

## Overview

This directory contains a complete, system-by-system analysis of the SparkEngine codebase. Every subsystem is documented with architecture descriptions, class/struct inventories, public API references, code examples, integration points, threading models, and design rationale.

SparkEngine is an open-source 3D game engine originally focused on first-person shooters, evolving into a general-purpose engine supporting FPS, RPG, MMO, open-world, and other genres.

## Document Index

| # | Document | Covers |
|---|----------|--------|
| 01 | [Architecture Overview](01-Architecture-Overview.md) | High-level architecture, module boundaries, startup sequence, design patterns |
| 02 | [Core Systems](02-Core-Systems.md) | Platform.h, EngineContext, EngineBootstrap, ModuleManager, EngineSettings, PluginRegistry |
| 03 | [Graphics & Rendering](03-Graphics-Rendering.md) | GraphicsEngine, Shader, RHI, RenderGraph, Lighting, Shadows, PostProcessing, Sky, Water, Terrain |
| 04 | [Physics System](04-Physics-System.md) | Jolt Physics, rigid bodies, constraints, raycasting, vehicles, ragdolls, cloth, soft bodies |
| 05 | [ECS System](05-ECS-System.md) | EnTT integration, 70+ components, 11 systems, SystemManager |
| 06 | [AI & Navigation](06-AI-Navigation.md) | Behavior trees, NavMesh, perception, steering, cover, formations, tactical points |
| 07 | [Animation System](07-Animation-System.md) | Skeletal animation, state machines, IK, blend spaces, root motion, compression |
| 08 | [Audio System](08-Audio-System.md) | XAudio2, AudioMixer, MusicManager, 3D spatial audio, reverb zones, DSP |
| 09 | [Networking System](09-Networking-System.md) | UDP client/server, AreaServer, WorldServer, replication, prediction, lag compensation |
| 10 | [Scripting System](10-Scripting-System.md) | AngelScript, Lua, hot-reload, script hooks |
| 11 | [World & Streaming](11-World-Streaming.md) | Origin rebasing, spatial grid, area streaming, day/night, proximity triggers |
| 12 | [Gameplay Systems](12-Gameplay-Systems.md) | Destruction, dialogue, events, loading, localization, modding, replay, save, UI, VR, coroutines |
| 13 | [Input System](13-Input-System.md) | InputManager, key bindings, gamepad, mouse capture |
| 14 | [Utilities](14-Utilities.md) | Logger, Console, Profiler, Assert, CVar, Timer, Math, FileUtils, CrashHandler |
| 15 | [SparkEditor](15-SparkEditor.md) | 32+ panels, collaborative editing, asset database, undo/redo, scene management |
| 16 | [SparkSDK](16-SparkSDK.md) | Public module API, IEngineContext, IModule, types |
| 17 | [SparkGame Module](17-SparkGame-Module.md) | Example FPS game, Player, ClassSystem, weapons, vehicles, enemies |
| 18 | [Build System & CI](18-Build-System-CI.md) | CMake, presets, feature toggles, GitHub Actions, 11 CI jobs |
| 19 | [Testing](19-Testing.md) | Custom test framework, 146 tests, assertion macros, coverage |

## Key Statistics

| Metric | Value |
|--------|-------|
| Source directories | 40+ |
| Header files | ~250+ |
| Implementation files | ~200+ |
| ECS Components | 70+ |
| ECS Systems | 11 core |
| Editor Panels | 32+ |
| Unit Tests | 146 files, 745+ test cases |
| CI Jobs | 11 (format, build, test, coverage, analysis) |
| Third-party Libraries | 13+ (EnTT, Jolt, ImGui, AngelScript, Recast, etc.) |
| Supported Platforms | Windows (primary), Linux, macOS (experimental) |
| Build Toggles | 14 feature flags |

## Architecture at a Glance

```
┌─────────────────────────────────────────────────────────────┐
│                      SparkEditor (ImGui)                      │
│  32 panels · collaborative editing · asset browser · undo     │
├─────────────────────────────────────────────────────────────┤
│                    SparkGame / Game Modules (DLL)              │
│  Player · Weapons · Enemies · Vehicles · GameModes            │
├─────────────────────────────────────────────────────────────┤
│                       SparkEngine (Core)                       │
│ ┌──────────┬──────────┬──────────┬──────────┬──────────────┐ │
│ │ Graphics │ Physics  │   ECS    │   AI     │  Animation   │ │
│ │ D3D11    │ Jolt     │ EnTT    │ BT+Nav   │  Skeletal    │ │
│ │ RHI      │ Vehicles │ 70+comp │ Steering │  IK/Blend    │ │
│ ├──────────┼──────────┼──────────┼──────────┼──────────────┤ │
│ │  Audio   │Networking│Scripting │Streaming │   World      │ │
│ │ XAudio2  │UDP C/S   │AngelScr  │Seamless  │ Origin Rebase│ │
│ │ Mixer    │Replicatn │Hot-reload│Area Mgr  │ Spatial Grid │ │
│ ├──────────┴──────────┴──────────┴──────────┴──────────────┤ │
│ │              Gameplay Systems Layer                        │ │
│ │  Destruction · Dialogue · Events · Save · UI · VR · etc.  │ │
│ ├───────────────────────────────────────────────────────────┤ │
│ │              Utilities & Core Infrastructure               │ │
│ │  Logger · Console · Profiler · CVar · Math · FileUtils    │ │
│ │  CrashHandler · Timer · Assert · ThreadSafeQueue · Hash   │ │
│ └───────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│  SparkConsole (ext process) · SparkShaderCompiler (tool)      │
│  SparkSDK (public headers) · Tests (146 test files)           │
└─────────────────────────────────────────────────────────────┘
```

## How to Use This Documentation

1. **New contributors**: Start with [01-Architecture-Overview](01-Architecture-Overview.md), then read the system relevant to your task
2. **Game developers**: Start with [16-SparkSDK](16-SparkSDK.md) and [17-SparkGame-Module](17-SparkGame-Module.md)
3. **Editor developers**: See [15-SparkEditor](15-SparkEditor.md)
4. **Debugging**: See [14-Utilities](14-Utilities.md) for logging, profiling, and console commands

## Cross-References

- **Wiki pages**: `wiki/` — 64 pages covering architecture, subsystems, and guides
- **API docs**: `docs/api/` — Auto-generated per-header API reference
- **Gap analyses**: `docs/gap-analysis/` — Feature completeness audits
